/*
 * Map.cpp
 *
 *  Created on: Jun 13, 2018
 *      Author: sujiwo
 */

#include <boost/archive/binary_oarchive.hpp>
#include <boost/archive/binary_iarchive.hpp>

#include <boost/graph/adj_list_serialize.hpp>

#include "VMap.h"
#include "KeyFrame.h"
#include "MapPoint.h"
#include "MapObjectSerialization.h"
#include "ImageDatabase.h"
#include "Frame.h"
#include "utilities.h"
#include "INIReader.h"



using namespace Eigen;
using namespace std;


VMap::VMap() :
	VMap(cv::Mat(), FeatureDetectorT::ORB, DescriptorMatcherT::BruteForce)
{}


VMap::VMap (const cv::Mat &_mask, FeatureDetectorT _fdetector, DescriptorMatcherT _dmatcher) :

	mask(_mask.clone()),
	featureDetector(VMap::createFeatureDetector(_fdetector)),
	descriptorMatcher(VMap::createDescriptorMatcher(_dmatcher)),

	keyframeInvIdx_mtx(new mutex),
	mappointInvIdx_mtx(new mutex)

{
	curDetector = _fdetector;
	curDescMatcher = _dmatcher;
	imageDB = new ImageDatabase(this);
}


VMap::~VMap()
{
	delete(imageDB);
}


void
VMap::setMask(cv::Mat m)
{ mask = m.clone(); }


int
VMap::addCameraParameter(const CameraPinholeParams &vsc)
{
	cameraList.push_back(vsc);
	return(cameraList.size() - 1);
}


Matrix<double,3,4> CameraPinholeParams::toMatrix() const
{
	Matrix<double,3,4> K = Matrix<double,3,4>::Zero();
	K(2,2) = 1.0;
    K(0,0) = fx;
    K(1,1) = fy;
    K(0,2) = cx;
    K(1,2) = cy;
	return K;
}


cv::Mat
CameraPinholeParams::toCvMat() const
{
	cv::Mat K = cv::Mat::zeros(3,3,CV_32F);
	K.at<float>(2,2) = 1.0;
    K.at<float>(0,0) = fx;
    K.at<float>(1,1) = fy;
    K.at<float>(0,2) = cx;
    K.at<float>(1,2) = cy;
	return K;
}


kfid VMap::createKeyFrame(const cv::Mat &imgSrc,
		const Eigen::Vector3d &p, const Eigen::Quaterniond &o,
		const int cameraId,
		KeyFrame **ptr,
		dataItemId sourceItemId,
		ptime tm)
{
	KeyFrame *nKf = new KeyFrame(imgSrc, p, o, mask, featureDetector, &cameraList[cameraId], cameraId, sourceItemId);
	nKf->parentMap = this;
	kfid nId = nKf->getId();

	keyframeInvIdx_mtx->lock();
	keyframeInvIdx.insert(pair<kfid,KeyFrame*> (nId, nKf));
	keyframeInvIdx_mtx->unlock();

	framePoints[nId] = map<mpid,kpid>();
	framePointsInv[nId] = map<kpid,mpid>();

//	imageDB->newKeyFrameCallback(nId);
	auto vtId = boost::add_vertex(covisibility);
	kfVtxMap[nId] = vtId;
	kfVtxInvMap[vtId] = nId;

	if (tm != boost::posix_time::not_a_date_time)
		nKf->setTimestamp(tm);

	if (ptr!=NULL)
		*ptr = nKf;
	return nId;
}


mpid VMap::createMapPoint(const Vector3d &p, MapPoint **ptr)
{
	MapPoint *mp = new MapPoint(p);
	mpid nId = mp->getId();
	mappointInvIdx.insert(pair<mpid,MapPoint*> (nId, mp));

	if (ptr!=NULL)
		*ptr = mp;
	return nId;
}


void VMap::estimateStructure(const kfid &kfid1, const kfid &kfid2)
{
	const KeyFrame
		*kf1 = getKeyFrameById(kfid1),
		*kf2 = getKeyFrameById(kfid2);

	vector<FeaturePair> featurePairs_1_2;
	KeyFrame::match(*kf1, *kf2, descriptorMatcher, featurePairs_1_2);

	vector<mpid> newMapPointList;
	KeyFrame::triangulate(kf1, kf2, newMapPointList, featurePairs_1_2,
		framePoints[kfid1], framePoints[kfid2],
		this);

	framePointsInv[kfid1] = reverseMap(framePoints[kfid1]);
	framePointsInv[kfid2] = reverseMap(framePoints[kfid2]);

	// Update visibility information
	for (mpid &mpidx: newMapPointList) {
		pointAppearances[mpidx].insert(kfid1);
		pointAppearances[mpidx].insert(kfid2);
	}
	updateCovisibilityGraph(kfid1);
}


double distance (const Vector2d &p1, const Vector2d &p2)
{
	Vector2d dx=p1-p2;
	return dx.norm();
}


void
VMap::estimateAndTrack (const kfid &kfid1, const kfid &kfid2)
{
	const KeyFrame
		*kf1 = getKeyFrameById(kfid1),
		*kf2 = getKeyFrameById(kfid2);

	// Track Map Points from KF1 that are visible in KF2
	kpidField kp1List = visibleField(kfid1);
	kpidField allKp2 = makeField(kfid2);
	vector<FeaturePair> pairList12;
	KeyFrame::matchSubset(*kf1, *kf2, descriptorMatcher, pairList12, kp1List, allKp2);
	map<kpid,mpid> &kf1kp2mp = framePointsInv[kfid1];

	// Check the matching with projection
	for (int i=0; i<pairList12.size(); i++) {
		auto &p = pairList12[i];
		const mpid ptId = kf1kp2mp[p.kpid1];

		// Try projection
		Vector2d kpf = kf2->project(getMapPointById(ptId)->getPosition());
		double d = distance(kpf, p.toEigen2());
		if (d >= 4.0)
			continue;

		// This particular mappoint is visible in KF2
		pointAppearances[ptId].insert(kfid2);
		framePoints[kfid2][ptId] = p.kpid2;
		framePointsInv[kfid2][p.kpid2] = ptId;
	}

	// Estimate new mappoints that are visible in KF1 & KF2
	kp1List.invert();
	kpidField kp2List = visibleField(kfid2);
	kp2List.invert();
	pairList12.clear();
	KeyFrame::matchSubset(*kf1, *kf2, descriptorMatcher, pairList12, kp1List, kp2List);
	vector<mpid> newMapPointList;
	KeyFrame::triangulate(kf1, kf2, newMapPointList, pairList12,
		framePoints[kfid1], framePoints[kfid2],
		this);

	// Update inverse map from map points to keypoints
	framePointsInv[kfid1] = reverseMap(framePoints[kfid1]);
	framePointsInv[kfid2] = reverseMap(framePoints[kfid2]);

	// Update visibility information
	for (mpid &mpidx: newMapPointList) {
		pointAppearances[mpidx].insert(kfid1);
		pointAppearances[mpidx].insert(kfid2);
	}
	updateCovisibilityGraph(kfid1);
}


vector<kfid>
VMap::allKeyFrames () const
{
	vector<kfid> kfIdList;
	for (auto &key: keyframeInvIdx) {
		kfIdList.push_back(key.first);
	}
	return kfIdList;
}


vector<mpid>
VMap::allMapPoints () const
{
	vector<mpid> mpIdList;
	for (auto &key: mappointInvIdx) {
		mpIdList.push_back(key.first);
	}
	return mpIdList;
}


pcl::PointCloud<pcl::PointXYZ>::Ptr
VMap::dumpPointCloudFromMapPoints ()
{
	pcl::PointCloud<pcl::PointXYZ>::Ptr mapPtCl
		(new pcl::PointCloud<pcl::PointXYZ>(mappointInvIdx.size(), 1));

	uint64 i = 0;
	for (auto &mpIdx: mappointInvIdx) {
		MapPoint *mp = mpIdx.second;
		mapPtCl->at(i).x = mp->X();
		mapPtCl->at(i).y = mp->Y();
		mapPtCl->at(i).z = mp->Z();
		i++;
	}

	return mapPtCl;
}


bool
VMap::save(const string &filepath)
{
	MapFileHeader header;
	header.numOfKeyFrame = keyframeInvIdx.size();
	header.numOfMapPoint = mappointInvIdx.size();
	header._descrptMt = curDescMatcher;
	header._featureDt = curDetector;

	cerr << "#KF: " << header.numOfKeyFrame << endl;
	cerr << "#MP: " << header.numOfMapPoint << endl;

	fstream mapFileFd;
	mapFileFd.open (filepath, fstream::out | fstream::trunc);
	if (!mapFileFd.is_open())
		throw runtime_error("Unable to create map file");
	mapFileFd.write((const char*)&header, sizeof(header));

	boost::archive::binary_oarchive mapStore (mapFileFd);

	mapStore << pointAppearances;
	mapStore << framePoints;
	mapStore << framePointsInv;

	mapStore << mask;

	mapStore << *imageDB;

	mapStore << cameraList;

	mapStore << covisibility;
	mapStore << kfVtxMap;
	mapStore << kfVtxInvMap;

	mapStore << keyValueInfo;

	for (auto &kfptr : keyframeInvIdx) {
		KeyFrame *kf = kfptr.second;
		mapStore << *kf;
	}

	for (auto &mpPtr : mappointInvIdx) {
		MapPoint *mp = mpPtr.second;
		mapStore << *mp;
	}

	mapFileFd.close();
	return true;
}


bool
VMap::load(const string &filepath)
{
	MapFileHeader header;

	fstream mapFileFd;
	mapFileFd.open(filepath.c_str(), fstream::in);
	if (!mapFileFd.is_open())
		throw runtime_error(string("Unable to open map file: ") + filepath);
	mapFileFd.read((char*)&header, sizeof(header));

	boost::archive::binary_iarchive mapStore (mapFileFd);

	KeyFrame *kfArray = new KeyFrame[header.numOfKeyFrame];
	MapPoint *mpArray = new MapPoint[header.numOfMapPoint];

	this->featureDetector = VMap::createFeatureDetector(header._featureDt);
	this->descriptorMatcher = VMap::createDescriptorMatcher(header._descrptMt);

	mapStore >> pointAppearances;
	mapStore >> framePoints;
	mapStore >> framePointsInv;

	mapStore >> mask;

	mapStore >> *imageDB;

	mapStore >> cameraList;

	mapStore >> covisibility;
	mapStore >> kfVtxMap;
	mapStore >> kfVtxInvMap;

	mapStore >> keyValueInfo;

	for (int i=0; i<header.numOfKeyFrame; i++) {
		mapStore >> kfArray[i];
	}
	for (int j=0; j<header.numOfMapPoint; j++) {
		mapStore >> mpArray[j];
	}

	// Rebuild pointers
	keyframeInvIdx.clear();
	for (int i=0; i<header.numOfKeyFrame; i++) {
		KeyFrame *kf = &(kfArray[i]);
		keyframeInvIdx.insert(pair<kfid,KeyFrame*>(kf->getId(), kf));
		kf->parentMap = this;
		kf->cameraParam = &(cameraList[kf->cameraId]);
	}

	mappointInvIdx.clear();
	for (int j=0; j<header.numOfMapPoint; j++) {
		MapPoint *mp = &(mpArray[j]);
		mappointInvIdx.insert(pair<mpid,MapPoint*>(mp->getId(), mp));
	}

	mapFileFd.close();
	return true;
}


map<mpid,kpid>
VMap::allMapPointsAtKeyFrame(const kfid f)
const
{
	if (framePoints.size()==0)
		return map<mpid,kpid>();

	return framePoints.at(f);
}



vector<pair<Vector3d,Quaterniond> >
VMap::dumpCameraPoses () const
{
	vector<pair<Vector3d,Quaterniond> > Retr;

	for (auto &kptr: keyframeInvIdx) {
		KeyFrame *kf = kptr.second;
		Retr.push_back(
			pair<Vector3d,Quaterniond>
				(kf->position(), kf->orientation()));
	}

	return Retr;
}


vector<kfid>
VMap::getKeyFrameList() const
{
	vector<kfid> ret;
	for (auto &kf: keyframeInvIdx)
		ret.push_back(kf.first);
	return ret;
}


std::vector<mpid>
VMap::getMapPointList() const
{
	vector<mpid> ret;
	for (auto &mp: mappointInvIdx)
		ret.push_back(mp.first);
	return ret;
}


kpidField
VMap::makeField (const kfid &kf)
{
	return kpidField (keyframe(kf)->numOfKeyPoints(), true);
}


kpidField
VMap::visibleField (const kfid &kf)
{
	kpidField field(keyframe(kf)->numOfKeyPoints(), false);
	for (auto &pt: framePoints[kf]) {
		const kpid &i = pt.second;
		field[(int)i] = true;
	}
	return field;
}


void
kpidField::invert()
{
	for (int i=0; i<size(); i++) {
		this->at(i) = !this->at(i);
	}
}


cv::Mat
kpidField::createMask (const kpidField &fld1, const kpidField &fld2)
{
	cv::Mat mask = cv::Mat::zeros(fld2.size(), fld1.size(), CV_8U);
	for (kpid i=0; i<fld2.size(); i++) {
		for (kpid j=0; j<fld1.size(); j++) {
			if (fld2[i]==true and fld1[j]==true)
				mask.at<char>(i,j) = 1;
		}
	}
	return mask;
}


uint
kpidField::countPositive() const
{
	uint n = 0;
	for (const kpid &v: *this) {
		if (v==true)
			n += 1;
	}
	return n;
}


cv::Ptr<cv::FeatureDetector>
VMap::createFeatureDetector(FeatureDetectorT fd)
{
	switch (fd) {
	case FeatureDetectorT::ORB:
		return cv::ORB::create(MAX_ORB_POINTS_IN_FRAME);
		break;
	case FeatureDetectorT::AKAZE:
		return cv::AKAZE::create();
		break;
	}
}


CameraPinholeParams
CameraPinholeParams::loadCameraParamsFromFile(const string &f)
{
	CameraPinholeParams c;
	INIReader cameraParser(f);
	c.fx = cameraParser.GetReal("", "fx", 0);
	c.fy = cameraParser.GetReal("", "fy", 0);
	c.cx = cameraParser.GetReal("", "cx", 0);
	c.cy = cameraParser.GetReal("", "cy", 0);
	c.width = cameraParser.GetInteger("", "width", 0);
	c.height = cameraParser.GetInteger("", "height", 0);

	return c;
}


CameraPinholeParams
CameraPinholeParams::operator* (const float r) const
{
	CameraPinholeParams n = *this;
	n.width *= r;
	n.height *= r;
	n.fx *= r;
	n.fy *= r;
	n.cx *= r;
	n.cy *= r;

	return n;
}


#include <cmath>

float
CameraPinholeParams::getHorizontalFoV() const
{
	double tanT = cx / fx;
	return 2 * atan(tanT);
}

float
CameraPinholeParams::getVerticalFoV() const
{
	double tanT = cy / fy;
	return 2 * atan(tanT);
}


void
VMap::reset()
{
	keyframeInvIdx.clear();
	mappointInvIdx.clear();
	pointAppearances.clear();
	framePoints.clear();
	framePointsInv.clear();
}


cv::Ptr<cv::DescriptorMatcher>
VMap::createDescriptorMatcher(DescriptorMatcherT dm)
{
	switch (dm) {
	case DescriptorMatcherT::BruteForce:
		// XXX: Should we activate cross-check for BFMatcher ?
		return cv::BFMatcher::create(cv::NORM_HAMMING2);
		break;
	}
}


void
VMap::updateCovisibilityGraph(const kfid k)
{
	map<kfid,int> kfCounter;

	for (auto mp_ptr: framePoints.at(k)) {
		mpid pId = mp_ptr.first;
		for (auto kr: pointAppearances.at(pId)) {
			if (kr==k)
				continue;
			kfCounter[kr]++;
		}
	}

	if (kfCounter.empty())
		return;

//	Should we clear the vertex?
//	boost::clear_vertex(kfVtxMap[k], covisibility);
	for (auto kfctr: kfCounter) {
		covisibilityGraphMtx.lock();
		// XXX: Do NOT put KfID directly to graph; use vertex descriptor instead
		boost::add_edge(kfVtxMap[k], kfVtxMap[kfctr.first], kfctr.second, covisibility);
		covisibilityGraphMtx.unlock();
	}
}


vector<kfid>
VMap::getKeyFramesComeInto (const kfid kTarget)
const
{
	vector<kfid> kfListSrc;

	auto kvtx = kfVtxMap.at(kTarget);
	auto k_in_edges = boost::in_edges(kvtx, covisibility);
	for (auto vp=k_in_edges.first; vp!=k_in_edges.second; ++vp) {
		auto v = boost::source(*vp, covisibility);
		int w = boost::get(boost::edge_weight_t(), covisibility, *vp);
		kfListSrc.push_back(kfVtxInvMap.at(v));
	}

	return kfListSrc;
}


vector<kfid>
VMap::getOrderedRelatedKeyFramesFrom (const kfid kx, int howMany) const
{
	vector<pair<KeyFrameGraph::vertex_descriptor,int>> covisk;

	auto vtx = kfVtxMap.at(kx);
	auto kfl = boost::out_edges(vtx, covisibility);
	for (auto p=kfl.first; p!=kfl.second; ++p) {
		auto k = boost::target(*p, covisibility);
		int w = boost::get(boost::edge_weight_t(), covisibility, *p);
		covisk.push_back(make_pair(k,w));
	}

	sort(covisk.begin(), covisk.end(),
		[](const pair<kfid,int> &u1, const pair<kfid,int> &u2) -> bool
		{ return u1.second > u2.second;}
	);

	vector<kfid> sortedKfs(covisk.size());
	for (int i=0; i<covisk.size(); i++) {
		sortedKfs[i] = kfVtxInvMap.at(covisk.at(i).first);
	}

	if (howMany<0 or sortedKfs.size()<howMany)
		return sortedKfs;
	else
		return vector<kfid> (sortedKfs.begin(), sortedKfs.begin()+howMany);
}


const int matchCountThreshold = 15;

void
VMap::trackMapPoints (const kfid kfid1, const kfid kfid2)
{
	const KeyFrame
		*kf1 = getKeyFrameById(kfid1),
		*kf2 = getKeyFrameById(kfid2);

	// Track Map Points from KF1 that are visible in KF2
	kpidField kp1List = visibleField(kfid1);
	kpidField allKp2 = makeField(kfid2);
	vector<FeaturePair> pairList12;
	KeyFrame::matchSubset(*kf1, *kf2, descriptorMatcher, pairList12, kp1List, allKp2);
	map<kpid,mpid> kf1kp2mp = framePointsInv[kfid1];

	// Check the matching with projection
	int pointMatchCounter = 0;
	for (int i=0; i<pairList12.size(); i++) {
		auto &p = pairList12[i];
		const mpid ptId = kf1kp2mp[p.kpid1];

		// Try projection
		Vector2d kpf = kf2->project(getMapPointById(ptId)->getPosition());
		double d = distance(kpf, p.toEigen2());
		if (d >= 4.0)
			continue;

		// This particular mappoint is visible in KF2
		pointMatchCounter += 1;
		pointAppearances[ptId].insert(kfid2);
		framePoints[kfid2][ptId] = p.kpid2;
		framePointsInv[kfid2][p.kpid2] = ptId;
	}

	if (pointMatchCounter > matchCountThreshold) {
		updateCovisibilityGraph(kfid1);
		cerr << "Backtrace: Found " << pointMatchCounter << "pts\n";
	}
}


kfid
VMap::search (const Frame &f) const
{
	return 0;
}


bool
VMap::removeMapPoint (const mpid &i)
{
	assert(mappointInvIdx.find(i) != mappointInvIdx.end());

	mappointInvIdx.erase(i);
	set<kfid> ptAppears = pointAppearances[i];
	for (auto k: ptAppears) {
		const kpid kp = framePoints[k].at(i);
		framePoints[k].erase(i);
//		framePointsInv[k].erase(kp);
	}

	pointAppearances.erase(i);

	// Modify Graph
	for (auto k: ptAppears) {
		updateCovisibilityGraph(k);
	}

	return true;
}


bool
VMap::removeMapPointsBatch (const vector<mpid> &mplist)
{
	set<kfid> kfModified;

	for (auto &pt: mplist) {
		mappointInvIdx.erase(pt);
		set<kfid> kfAppears = pointAppearances[pt];
		for (auto kf: kfAppears) {
			kfModified.insert(kf);
			const kpid kp = framePoints[kf].at(pt);
			framePoints[kf].erase(pt);
		}

		pointAppearances.erase(pt);
	}

	for (auto kf: kfModified) {
		updateCovisibilityGraph(kf);
	}

	return true;
}


void
VMap::fixFramePointsInv ()
{
	framePointsInv.clear();

	for (auto &mpPtr: framePoints) {
		const kfid &kf = mpPtr.first;
		framePointsInv[kf] = reverseMap(framePoints[kf]);
	}

}