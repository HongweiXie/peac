//
// Copyright 2014 Mitsubishi Electric Research Laboratories All
// Rights Reserved.
//
// Permission to use, copy and modify this software and its
// documentation without fee for educational, research and non-profit
// purposes, is hereby granted, provided that the above copyright
// notice, this paragraph, and the following three paragraphs appear
// in all copies.
//
// To request permission to incorporate this software into commercial
// products contact: Director; Mitsubishi Electric Research
// Laboratories (MERL); 201 Broadway; Cambridge, MA 02139.
//
// IN NO EVENT SHALL MERL BE LIABLE TO ANY PARTY FOR DIRECT,
// INDIRECT, SPECIAL, INCIDENTAL, OR CONSEQUENTIAL DAMAGES, INCLUDING
// LOST PROFITS, ARISING OUT OF THE USE OF THIS SOFTWARE AND ITS
// DOCUMENTATION, EVEN IF MERL HAS BEEN ADVISED OF THE POSSIBILITY OF
// SUCH DAMAGES.
//
// MERL SPECIFICALLY DISCLAIMS ANY WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE. THE SOFTWARE PROVIDED HEREUNDER IS ON AN
// "AS IS" BASIS, AND MERL HAS NO OBLIGATIONS TO PROVIDE MAINTENANCE,
// SUPPORT, UPDATES, ENHANCEMENTS, OR MODIFICATIONS.
//
#pragma warning(disable: 4996)
#pragma warning(disable: 4819)
#define _CRT_SECURE_NO_WARNINGS

#include <pcl/point_types.h>
#include <pcl/io/openni_grabber.h>
#include <pcl/console/parse.h> //zc
#include <pcl/point_cloud.h> //zc: ���� 1.8֮��, �ƺ�������ʽ�Ӵ���

#include "opencv2/opencv.hpp"

#include "AHCPlaneFitter.hpp"

using ahc::utils::Timer;

#include "zcAhcUtility.h"

using ahc::PlaneSeg;
using std::vector;
using std::string;
using std::cout;
using std::endl;
using std::ofstream;

using std::set; //��������/ƽ�й�ϵ����
using std::set_intersection;
using namespace Eigen;

// typedef Eigen::Matrix<float, 3, 3, Eigen::RowMajor> Matrix3frm; //cube-pose һ��ӦΪĬ��columnMajor
// typedef Eigen::Matrix<double, 3, 3, Eigen::RowMajor> Matrix3drm;

//const float MM2M = 1e-3;
const float cos30 = 0.8660254; //cos(30��)

bool dbg1_ = false; //���ڵ������ seg ͼ, �Լ�����ѭ�����
bool dbg2_ = false; //���ڵ������ debug print

string deviceId_ = ""; //zc: �����в��� -dev
double fx_ = 525.5, fy_ = 525.5, cx_ = 320, cy_ = 240; //�ڲ�: ������ -intr
vector<string> pngFnames_;
int png_sid_ = 0; //start id ������

bool isQvga_ = false;
int qvgaFactor_ = 1; //if isQvga_=true, --> factor=2

//��--TUM Ĭ�ϷŴ� 5(˵��5000) �洢���ڹ۲�: http://vision.in.tum.de/data/datasets/rgbd-dataset/file_formats#color_images_and_depth_maps
//������һЩ��������ԭʼ ushort, �ʼ��ؾ�����ʱ, �� -dscale 1 (Ĭ��), ���� TUM-dataset ʱ, �� -dscale 5
float dscale_ = 1;

//vector<vector<double>> cubePoses_; //����, �ؼ���Ҫ���������̬����
vector<vector<double>> cubeCandiPosesPrev_; //ǰ��һ֡��(�Ƕ�֡����)������ĺ�ѡ����ϵ; //�Ƿ� vector<Affine3d> �õ�? ��ʱ���ø��� 2016-9-27 16:11:59
static bool isFirstFrame_ = true; //if true, cubePosePrev_=curr...
int poseIdxPrev_ = 0; //cubePosesPrev_ �����ĸ�idx����������ο�ϵ		//�����á�, ��Ϊ���������������ĸ�ֱ�����ο�ϵ, 

string poseFn_ = ""; //�����̬�����ļ�
vector<Affine3d, aligned_allocator<Affine3d>> camPoses_;
int lastValidIdx_ = -1; //camPoses_ �ĳ���ȫ����ЧFLAG֮��, �˱����洢�ϴ���Чλ�� idx //2016-12-9 00:14:51

#define SINGLE_VERT 0
#define PROCRUSTES 01


//@author zhangxaochen, ������ zc::getFnamesInDir@kinfu_app.cpp
//@brief get file names in *dir* with extension *ext*
//@param dir file directory
//@param ext file extension
vector<string> getFnamesInDir(const string &dir, const string &ext){
	namespace fs = boost::filesystem;
	//fake dir:
	//cout << fs::is_directory("a/b/c") <<endl; //false

	//fs::path dirPath(dir); //no need
	cout << "path: " << dir << endl
		<< "ext: " << ext <<endl;

	if(dir.empty() || !fs::exists(dir) || !fs::is_directory(dir)) //��ʵ is_directory ������ exists: http://stackoverflow.com/questions/2203919/boostfilesystem-exists-on-directory-path-fails-but-is-directory-is-ok
		PCL_THROW_EXCEPTION (pcl::IOException, "ZC: No valid directory given!\n");

	vector<string> res;
	fs::directory_iterator pos(dir),
		end;

	for(; pos != end; ++pos){
		if(fs::is_regular_file(pos->status()) && fs::extension(*pos) == ext){
#if BOOST_FILESYSTEM_VERSION == 3
			res.push_back(pos->path().string());
#else
			res.push_back(pos->path());
#endif
		}
	}

	if(res.empty())
		PCL_THROW_EXCEPTION(pcl::IOException, "ZC: no *" + ext + " files in current directory!\n");
	return res;
}//getFnamesInDir

//zc: OrganizedImage3DΪ�㷨������ȥʵ�ֵĶ�̬��, û�붮: Ϊʲô���ô������? (�����и� NullImage3D ��ʾ���ࡱ) //2016-9-11 16:16:02
// pcl::PointCloud interface for our ahc::PlaneFitter
template<class PointT>
struct OrganizedImage3D {
	const pcl::PointCloud<PointT>& cloud;
	//NOTE: pcl::PointCloud from OpenNI uses meter as unit,
	//while ahc::PlaneFitter assumes mm as unit!!!
	const double unitScaleFactor;

	OrganizedImage3D(const pcl::PointCloud<PointT>& c) : cloud(c), unitScaleFactor(1000) {}
	int width() const { return cloud.width; }
	int height() const { return cloud.height; }
	bool get(const int row, const int col, double& x, double& y, double& z) const {
		const PointT& pt=cloud.at(col,row);
		x=pt.x; y=pt.y; z=pt.z;
		return pcl_isnan(z)==0; //return false if current depth is NaN
	}
};
typedef pcl::PointXYZRGBA PtType;
typedef OrganizedImage3D<PtType> RGBDImage;
typedef ahc::PlaneFitter<RGBDImage> PlaneFitter;

class MainLoop
{
protected:
	PlaneFitter pf;
	cv::Mat rgb, seg;
	bool done;

public:
	bool pause_; //zc
	pcl::OpenNIGrabber* grabber_;

public:
	MainLoop () : done(false), pause_(false) {}

	//process a new frame of point cloud
	void onNewCloud (const pcl::PointCloud<PtType>::ConstPtr &cloud)
	{
		//fill RGB
		if(rgb.empty() || rgb.rows!=cloud->height || rgb.cols!=cloud->width) {
			rgb.create(cloud->height, cloud->width, CV_8UC3);
			seg.create(cloud->height, cloud->width, CV_8UC3);
		}
// 		const PtType &tmppt = cloud->at(520,360);
// 		std::cout<<"xyz:= "<<tmppt.x<<", "<<tmppt.y<<", "<<tmppt.z<<std::endl; //�۲��ڲ��Ƿ���3D������Ӱ��, Ӧ����, ���ƺ�ʵ��û��, bug?

		for(int i=0; i<(int)cloud->height; ++i) {
			for(int j=0; j<(int)cloud->width; ++j) {
				const PtType& p=cloud->at(j,i);
				if(!pcl_isnan(p.z)) {
					rgb.at<cv::Vec3b>(i,j)=cv::Vec3b(p.b,p.g,p.r);
				} else {
					rgb.at<cv::Vec3b>(i,j)=cv::Vec3b(255,255,255);//whiten invalid area
				}
			}
		}

		//run PlaneFitter on the current frame of point cloud
		RGBDImage rgbd(*cloud);
		Timer timer(1000);
		timer.tic();
		//pf.run(&rgbd, 0, &seg);
		vector<vector<int>> idxss;
		if(dbg1_)
			pf.run(&rgbd, &idxss, &seg);
		else
			//pf.run(&rgbd, &idxss, &seg, 0, false);
			pf.run(&rgbd, &idxss, 0, 0, false); //�� seg Ҳʡȥ��Ҫ, �ƺ�����ʡʱ��
		double process_ms=timer.toc();

		if(dbg1_)
			annotateLabelMat(pf.membershipImg, &seg); //release: 6ms
		//cv::imwrite("shit.png", seg); //release: 5ms

		vector<PlaneSeg> plvec; //��Ÿ���ƽ�����
		timer.tic();
#if 01	//zcRefinePlsegParam ���, ��ȡ�ɺ���
		for(size_t i=0; i<idxss.size(); i++){
			vector<int> &idxs = idxss[i];
			PlaneSeg tmpSeg(rgbd, idxs);
			plvec.push_back(tmpSeg);

			if(dbg2_)
				printPlaneParams(tmpSeg);

			//zc: old/new �Ա�, ��refineЧ�� //��ʵ���, 2016-9-20 15:09:57
			//PlaneSeg &oldSeg = *pf.extractedPlanes[i];
			//double *oldNorm = oldSeg.normal;
			//double *oldCen = oldSeg.center;
			//double oldCurv = oldSeg.curvature;
			//double oldMse = oldSeg.mse;

			//double *newNorm = tmpSeg.normal;
			//double *newCen = tmpSeg.center;
			//double newCurv = tmpSeg.curvature;
			//double newMse = tmpSeg.mse;
			//printf("norm--old.dotProd(new)==%f; center.dist==%f; o/nCurv=(%f, %f), o/nMse==(%f, %f)\n", dotProd(oldNorm, newNorm, 3), dist(oldCen, newCen), oldCurv, newCurv, oldMse, newMse);
		}
#else
		for(size_t i=0; i<pf.extractedPlanes.size(); i++)
			plvec.push_back(*pf.extractedPlanes[i]);

		plvec = zcRefinePlsegParam(rgbd, idxss); //�����Ƿ� refine ƽ�����
#endif	//zcRefinePlsegParam ���, ��ȡ�ɺ���

		if(dbg1_)
			timer.toc("re-calc-plane-equation:"); //debug:7ms; release:1.5ms

		//zc: ��ʴ������Ƭmsk, ������Ա�normalƫ��
#if 0	//���� krnl=5,7, norm-angle<0.3��,˵��erode���岻��, ���ʡȥ 2016-9-21 17:14:56
		timer.tic();
		vector<PlaneSeg> plvecErode;
		int krnlSz=7;
		cv::Mat erodeKrnl = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(krnlSz,krnlSz));
		for(size_t i=0; i<idxss.size(); i++){
			cv::Mat msk = (pf.membershipImg == i);
			cv::erode(msk, msk, erodeKrnl);
			PlaneSeg tmpSeg(rgbd, msk);
			plvecErode.push_back(tmpSeg);

			double cosNorm = dot(plvec[i].normal, tmpSeg.normal);
			printf("i= %d, cosNorm= %f, angle= %f\n", i, cosNorm, RAD2DEG(acos(cosNorm)));
		}
		timer.toc("erode-calc-plane-params:"); //release:5.4ms(erode:3ms)
#endif

		timer.tic();
		vector<vector<double>> cubeCandiPoses; //��vec����size=12=(t3+R9), ��cube���������ϵ����̬, �ҹ涨row-major; ��vec��ʾ�����ѡ���ǵ���̬����

#if 01	//zcFindOrtho3tup���, ��ȡ�ɺ���
		//zc: ����������������ƽ��, ���һ����Ԫ�� (�����ж���, e.g., ������һ����ֱ������¶���, ����������), 
		//ʾ��ͼ��: http://codepad.org/he35YCTh
		//1. �������ƽ��, ���������ƽ�й�ϵ, �Ȳ�����Ҳ��ƽ�еĺ���
		//size_t plcnt = plvec.size();
		size_t plcnt = idxss.size();
		double orthoThresh = 
			//0.0174524; //cos(89��), С�ڴ�Ϊ���� //��Ϊ���� pcl160, ��OpenNIGrabber��Ĭ���ڲ�,���µ��Ʋ�׼, ������ﲻ������ֵ
			0.0871557; //cos(85��) �ſ�, //����������ڲ�֮��, ���ַǳ���, ������Ȼ�ſ�
		double paralThresh = 
			//0.999847695; //cos(1��), ���ڴ�Ϊƽ��
			0.996194698; //cos(5��) �ſ�

		//vector<vector<int>> orthoMap(plcnt); //������ϵ��, ������ֻ���������, �����һ��vecӦ�ǿ�; ����ʵ vector<set<int>> ������, �ݲ���
		//vector<vector<int>> paralMap(plcnt); //ƽ�й�ϵ��
		//for(size_t i=0; i<plcnt; i++){
		//	PlaneSeg &pl_i = plvec[i];
		//	double *norm_i = pl_i.normal;

		//	for(size_t j=i+1; j<plcnt; j++){ //�� i+1, ֻ��������
		//		PlaneSeg &pl_j = plvec[j];
		//		double *norm_j = pl_j.normal;
		//		double cosNorm = dot(norm_i, norm_j, 3); //�� |a|=|b|=1, ��ֱ�� cos(a,b)=a.dot(b)

		//		if(cosNorm < orthoThresh)
		//			orthoMap[i].push_back(j);
		//		if(cosNorm > paralThresh)
		//			paralMap[i].push_back(j);
		//	}//for-j
		//}//for-i

		////2. ��ÿ�� orthoMap[i] ��, ������������, ���ҵ�һ����Ԫ��!
		//for(size_t i=0; i<orthoMap.size(); i++){
		//	vector<int> &ortho_i = orthoMap[i]; //��id=i������ƽ��id��
		//	for(size_t j=0; j<ortho_i.size(); j++){
		//		int oij = ortho_i[j];
		//		vector<int> &ortho_j = orthoMap[oij]; //�̶�i,j֮��
		//		for(size_t k=j+1; k<ortho_i.size(); k++){

		//		}
		//	}
		//}

		vector<set<int>> orthoMap(plcnt);  //������ϵ��, ������ֻ���������, �����һ��vecӦ�ǿ�; ����ʵ vector<set<int>> ������, �ݲ���
		vector<vector<int>> ortho3tuples; //�ҵ�����Ԫ�������, ��vec ������ vec ���� set, ��������; ��vec��Ȼsize=3
		vector<set<int>> paralMap(plcnt); //ƽ�й�ϵ��
		for(size_t i=0; i<plcnt; i++){
			PlaneSeg &pl_i = plvec[i];
			double *norm_i = pl_i.normal;
			for(size_t j=i+1; j<plcnt; j++){ //�� i+1, ֻ��������
				PlaneSeg &pl_j = plvec[j];
				double *norm_j = pl_j.normal;
				double cosNorm = dotProd(norm_i, norm_j, 3); //�� |a|=|b|=1, ��ֱ�� cos(a,b)=a.dot(b)

				if(dbg2_)
					printf("i,j=(%u,%u), cosNorm=%f; angle=%f\n", i, j, cosNorm, RAD2DEG(acos(cosNorm)));

				if(abs(cosNorm) < orthoThresh)
					orthoMap[i].insert(j);
				if(abs(cosNorm) > paralThresh)
					paralMap[i].insert(j);
			}//for-j
		}//for-i

		//2. ��ÿ�� orthoMap[i] ��, ������������, ���ҵ�һ����Ԫ��! //�����ϡ���֪�����塿��ֻ���ҵ�������Ԫ��, ��>2, ��Ҫͨ��ƽ���������ų�
		for(size_t i=0; i<orthoMap.size(); i++){
			set<int> &ortho_i = orthoMap[i]; //��id=i������ƽ��id��
			if(ortho_i.size() < 2) //�� <2, �򹹲�����Ԫ��
				continue;
			set<int>::const_iterator iter_j = ortho_i.begin();
			for(; iter_j!=ortho_i.end(); iter_j++){
				int idx_j = *iter_j;
				set<int> &ortho_j = orthoMap[idx_j]; //�����ڴ��� idx_k, ���ҵ�, ���һ����Ԫ��
				if(ortho_j.size() == 0)
					continue;
				//set<int>::const_iterator iter_k = iter_j + 1; //��, �� '+'
				//set<int>::const_iterator iter_k = iter_j; iter_k++; //��, ����ԭʼ
				set<int>::const_iterator iter_k = std::next(iter_j);
				for(; iter_k!=ortho_i.end(); iter_k++){
					int idx_k = *iter_k;
					if(ortho_j.count(idx_k)){ //�ҵ���Ԫ��
						vector<int> tuple3;
						tuple3.push_back(i);
						tuple3.push_back(idx_j);
						tuple3.push_back(idx_k);
						ortho3tuples.push_back(tuple3);
					}
				}//for-iter_k
			}//for-iter_j
		}//for-i

		//3. �� ortho3tuples ���ܴ��ڡ��ٵġ���Ԫ��, �ж�����: ����ʵ�����ڲ�����Ԫ��-->����Ϊ: �����������潻��(3D)��labelMat(2D)��ĳlen��������������Чlabel��������
		//�ж����ݸ�Ϊ: ����label-set ���� tuple3-vec (��Ȼһ�� set һ�� vector, �� std::includes �㷨) //2016-12-7 20:55:25
		vector<vector<int>> tmp3tuples;
		for(size_t i=0; i<ortho3tuples.size(); i++){
			vector<int> &tuple3 = ortho3tuples[i];
			//1. ȡ����, ���� Ax=b �� [A|b] �������; ����Ԫ��˹, ������涥��
			vector<vector<double>> matA;
			vector<double> matb;
			for (int ii=0; ii<3; ii++){
				int plIdx = tuple3[ii];
				PlaneSeg &plseg = plvec[plIdx];
				//ƽ�����ABCD: (ABC)=normal; D= -dot(normal, center) //ע�⸺��, b[i]=-D=dot...
				vector<double> tmpRow(plseg.normal, plseg.normal+3);
				double b_i = dotProd(plseg.normal, plseg.center);
				tmpRow.push_back(b_i); //ϵ������һ��, ���� Ai|bi
				matA.push_back(tmpRow);
			}
			vector<double> vertx; //���潻��, ������Ľ�; �߶��ǲ�����(m)���� ��
			RGauss(matA, vertx);

			//2. ����������������Ƿ���������Чlabel
			//3D->2D���ص�, ��int, ��׷�󾫶�
			int u = (vertx[0] * fx_) / vertx[2] + cx_,
				v = (vertx[1] * fy_) / vertx[2] + cy_;

			int winSz = 20 / qvgaFactor_; //���򴰿ڳ���
			cv::Rect tmpRoi(u - winSz/2, v - winSz/2, winSz, winSz);
			if(tmpRoi != (tmpRoi & cv::Rect(0,0, seg.cols, seg.rows)) ) //��������ͼ��Χ��, ����
				continue;
			//else: ����, ����������С������ͼ����, ����
			if(dbg1_){
				cv::circle(seg, cv::Point(u, v), 2, 255, 1); //��СԲȦ
				cv::circle(seg, cv::Point(u, v), 7, cv::Scalar(0,0,255), 2); //���ԲȦ, ͬ��Բ, ���Թ۲����
			}

			cv::Mat vertxNbrLmat(pf.membershipImg, tmpRoi); //���� label-mat
			vertxNbrLmat = vertxNbrLmat.clone(); //clone �ܽ��set(labelMat) ����������? �� ��!
					//�ǲ�����, Ӧ����˵, ���� clone, ��roi����view���������ڴ�, ȡ [data, data+size] ʱ��ȡ��ԭ Mat һ��Ƭ��(�� 1*16), ����������Ҫ�ķ���(��4*4) //2016-12-28 11:22:14
			//cout<<"vertxNbrLmat:\n"<<vertxNbrLmat<<endl;
			//cv::Mat dbgRoiMat(seg, tmpRoi); //���Թ۲�С����

			int *vertxNbrLmatData = (int*)vertxNbrLmat.data; //label mat raw data
			set<int> nbrLabelSet(vertxNbrLmatData, vertxNbrLmatData + winSz * winSz);
			//int posCnt = 0; //���� label>0 ͳ����
			//for(set<int>::const_iterator it = nbrLabelSet.begin(); it != nbrLabelSet.end(); it++){
			//	if(*it >= 0) //0Ҳ����Чlabel
			//		posCnt++;
			//}
			if(std::includes(nbrLabelSet.begin(), nbrLabelSet.end(), tuple3.begin(), tuple3.end()) ){ //����֤, Ч���ܺ� 2016-12-9 00:09:40
			//if(posCnt >= 3){ //�϶�Ϊ��ʵ������������Ԫ��
				if(dbg1_){
					cv::circle(seg, cv::Point(u, v), 2, 255, 1); //��СԲȦ
					cv::circle(seg, cv::Point(u, v), 7, cv::Scalar(0,255,0), 2); //�̴�ԲȦ, ͬ��Բ, ���Թ۲����, //��ʾɸѡ���ն��µĶ���
				}

				tmp3tuples.push_back(tuple3);
				cubeCandiPoses.push_back(vertx); //�Ȱ�(R,t)��t���; ֮������ cubePoses ��Ҫ push, Ҫ��ÿ�� .insert(.end, dat, dat+3);
			}
		}//for-ortho3tuples
		ortho3tuples.swap(tmp3tuples);

		if(cubeCandiPoses.size() == 2){
			Vector3d pt0(cubeCandiPoses[0].data());
			Vector3d pt1(cubeCandiPoses[1].data());
			
			printf("���������Ǿ��룺%f\n", (pt0 - pt1).norm());
		}

		//4. �������桢���߶�λ������; ��һ֡�ж����Ԫ�飬�򶼼��㣬�������࣬������һ֡��Ԫ����ٶ�����
		for(size_t i=0; i<ortho3tuples.size(); i++){
			vector<int> &tuple3 = ortho3tuples[i];
			//vector<double> pose_i; //�����ɵĺ�ѡ��̬������֮һ; �˴��� R; ǰ���Ѿ�push�� t(=vertx)
			vector<double> ax3orig; //��ʼ����, ����������
			ax3orig.reserve(9);

			//4.1. �������: ������δ������, Ҫ�á�ʩ������������, Ҫ��ѡ����, �����������ڴ�; ����δ����cube��xyz��һ��, ȷ��xyzҪ�����᳤��
#if 0	//v1: ������Z��(��O->plCenter����)�н�С(��abs(normal.z)���)��������; �ڶ�С�����ڶ���; 1,2�����������
			//�����᣺
			double maxAbsZ = 0, minorAbsZ = 0;
			int mainPlIdx = -1, minorPlIdx = -1;
			for(size_t j=0; j<3; j++){
				int plIdx = tuple3[j];
				PlaneSeg &plseg = plvec[plIdx];
				double absz = abs(plseg.normal[2]);
				if(absz > maxAbsZ){
					maxAbsZ = absz;
					//minorPlIdx = mainPlIdx; //�ڶ������ϴε����� //�߼���
					mainPlIdx = plIdx;
				}
				if(absz < maxAbsZ && absz > minorAbsZ){
					minorAbsZ = absz;
					minorPlIdx = plIdx;
				}
			}//for-tuple3

			double *mainAxis = plvec[mainPlIdx].normal;
			double *minorAxis = plvec[minorPlIdx].normal;

#if 0	//v1.1: ����������
			double *ax1 = new double[3];
			double *ax2 = new double[3];
			double *ax3 = new double[3];
			schmidtOrtho(mainAxis, minorAxis, ax1, ax2, 3);
			crossProd(ax1, ax2, ax3); //��Ϊ���, ax1,2,3��Ȼ������ϵ, ����ת����
			pose_i.insert(pose_i.end(), ax1, ax1 + 3);
			pose_i.insert(pose_i.end(), ax2, ax2 + 3);
			pose_i.insert(pose_i.end(), ax3, ax3 + 3);
#elif 1	//v1.2: �á�һ��������洢
			//double *axs = new double[9]; //�α�new��?
			double axs[9];
			schmidtOrtho(mainAxis, minorAxis, axs, axs+3, 3);
			crossProd(axs, axs+3, axs+6); //���ɵ�����
			pose_i.insert(pose_i.end(), axs, axs+9);
#endif	//ʩ����

#elif 1	//v2: ����������, ��: https://www.evernote.com/shard/s399/nl/67976577/48135b5e-7209-47c1-9330-934ac4fee823
#if 01	//v2.1 ���桾��������, ������û��ϵ
			for(size_t kk=0; kk<3; kk++){
				double *pl_k_norm = plvec[tuple3[kk]].normal;
				ax3orig.insert(ax3orig.end(), pl_k_norm, pl_k_norm+3);
			}
#elif 1	//v2.2 ���桾���ߡ�����,	//���������̬��Ȼ�����桾�������᡿ȫ��!!! ��δ�Ƶ�, ������Դ�������֤ -��
			//����ֻ��Ҫ�ܷ���, ��Ϊ����֮ǰ�Ѿ� RGauss �����; ��������������
			for(size_t ii=0; ii<3; ii++){
				double *norm_i = plvec[tuple3[ii]].normal;
				for (size_t jj=ii+1; jj<3; jj++){
					double *norm_j= plvec[tuple3[jj]].normal;
					double intersLine[3];
					crossProd(norm_i, norm_j, intersLine);
					ax3orig.insert(ax3orig.end(), intersLine, intersLine+3);
				}
			}
#endif //����/���� ˭����ʼ��

			//Matrix3d ttmp = Map<Matrix3d>(ax3orig); //�����, ���� .data()
			//JacobiSVD<Matrix3d> svd(Map<Matrix3d>(ax3orig)); //��, ��Ȼ�����, ���Ǽٵ�, ע�� warning C4930: prototyped function not called
			//Matrix3d tmp = Map<Matrix3d>(ax3orig.data()); //ok
			//JacobiSVD<Matrix3d> svd(tmp, ComputeThinU | ComputeThinV); //���д�, JacobiSVD: thin U and V are only available when your matrix has a dynamic number of columns
			JacobiSVD<Matrix3d> svd(Map<Matrix3d>(ax3orig.data()), ComputeFullU | ComputeFullV);
			Matrix3d svdU = svd.matrixU();
			Matrix3d svdV = svd.matrixV();
			Matrix3d orthoAxs = svdU * svdV.transpose(); //����õ����� ax3orig �����Ż�������, det=��1, ��ȷ������ת����
			double *axs = orthoAxs.data();
			//pose_i.insert(pose_i.end(), axs, axs+9);

			//+++++++++++++++tmp: ���ҷ����� ���������᡿ȫ�ȵ�ԭ��	//��:���Ż�֮��, ����������ȷʵȫ��
// 			{
// 			vector<double> tmp;
// 			for(size_t ii=0; ii<3; ii++){
// 				double *norm_i = plvec[tuple3[ii]].normal;
// 				for (size_t jj=ii+1; jj<3; jj++){
// 					double *norm_j= plvec[tuple3[jj]].normal;
// 					double intersLine[3];
// 					crossProd(norm_i, norm_j, intersLine);
// 					tmp.insert(tmp.end(), intersLine, intersLine+3);
// 				}
// 			}
// 			JacobiSVD<Matrix3d> svd(Map<Matrix3d>(tmp.data()), ComputeFullU | ComputeFullV);
// 			Matrix3d svdU = svd.matrixU();
// 			Matrix3d svdV = svd.matrixV();
// 			Matrix3d orthoAxs = svdU * svdV.transpose(); //����õ����� ax3orig �����Ż�������, det=��1, ��ȷ������ת����
// 			double *axs = orthoAxs.data();
// 			}
		
#endif //���ֲ�ͬ��������ʽ

			//v3: ��������, �� procrustes���⴦��: https://en.wikipedia.org/wiki/Orthogonal_Procrustes_problem
#if PROCRUSTES
			axs = ax3orig.data();
#endif //PROCRUSTES

			//cubeCandiPoses[i].insert(cubeCandiPoses[i].end(), pose_i.begin(), pose_i.end());
			cubeCandiPoses[i].insert(cubeCandiPoses[i].end(), axs, axs+9); //���� rawָ��������
		}//for-ortho3tuples
#else
		zcFindOrtho3tup(plvec, pf.membershipImg, fx_, fy_, cx_, cy_, cubeCandiPoses, seg);
#endif	//zcFindOrtho3tup���, ��ȡ�ɺ���
		//---------------Ŀǰ�߼�: 
		//1. �����ʼ֡�Ҳ�����������; 
		//2. ���Ҳ�����, �� �˴� tvec=(1e3,1e3,1e3)����ֵ, �� ������� csv ��ȫ��, ��Ϊ��Ч��ʶ
		//3. ���ҵ����ǵĵ�һ֡��ʼ, ����Чֵ (1.5,1.5, -0.3), ���Ҵ��������ϵ ��Ϊ֮������֡��ȫ�ֲο�ϵ
		//4. ���м�ĳ(i)֡����, ���µ�(j)�ҵ�֮��, ����̬����(i)������

		//��ǰ֡�Ƿ��ҵ�����һ�����ǣ� //2016-12-8 14:44:29
		bool isFindOrthoCorner = (cubeCandiPoses.size() != 0);
		if(!isFindOrthoCorner){ //������������, ����������̬��Ϊ��Чֵ
			cout << "NO-CORNER-FOUND~" << endl;
			Affine3d invalidPose;
			invalidPose.linear() = Matrix3d::Identity();
			invalidPose.translation() << 1e3, 1e3, 1e3; //���� t �ǳ�����Ϊ��Чֵ FLAG

			camPoses_.push_back(invalidPose);
		}
		else{ //isFindOrthoCorner==true
			cout << "isFindOrthoCorner==true" << endl;

			if(isFirstFrame_){
				isFirstFrame_ = false;

				Affine3d initPose;
				//init_Rcam_ = Eigen::Matrix3f::Identity ();// * AngleAxisf(-30.f/180*3.1415926, Vector3f::UnitX());
				//init_tcam_ = volume_size * 0.5f - Vector3f (0, 0, volume_size (2) / 2 * 1.2f);
				//��--������@kinfu.cpp
				initPose.linear() = Matrix3d::Identity();
				initPose.translation() << 1.5, 1.5, -0.3;

				camPoses_.push_back(initPose);
			}
			else{ //isFirstFrame_==false
				//+++++++++++++++TODO
				//��ֹ�� cubePosesPrev_[poseIdxPrev_] �Ա�, ��Ϊǰһ֡�е�, ��ǰ֡���ܸ���; Ҫ����, ��prev&currƥ���ֱ��
				size_t i_match, //curr
					j_match; //prev
				
				//���� i/j_match �� ������ʱ�õ�, ���öඥ����, ����� match-ind-vec, ���Ա�������	//2016-12-14 15:56:00
				vector<size_t> i_match_vec, j_match_vec; //�˶��� size ��Ȼ��ͬ
				
				bool foundMatch = false;

				vector<double> multiRmats_; //���ඥ�Ƕ�λ (!SINGLE_VERT), ���ô�, ����=9*k; //��Ҫ��ȫ�ֱ���, ����ѭ������
				vector<double> multiRmatsPrev_;

				for(size_t i=0; i<cubeCandiPoses.size(); i++){
					vector<double> &rt = cubeCandiPoses[i];
					double *pRcurr = rt.data() + 3;

					//size_t j;
					for(size_t j=0; j<cubeCandiPosesPrev_.size(); j++){
						vector<double> &rtPrev = cubeCandiPosesPrev_[j];
						double vertsDist = dist(rt.data(), rtPrev.data()); //poses[i]-posesPrev[j] ����t���, data()ȡָ��
						if(vertsDist < 0.05){ //��λ��(�Ǻ���), 5cm; ��ǰ����֡ĳ����ѡ�����С����ֵ, ���˳�ѭ������;
							foundMatch = true;

							//�����ǲ��� //���� //2016-12-14 15:57:12
							i_match = i;
							j_match = j;

							//�ඥ�ǲ���
							i_match_vec.push_back(i);
							j_match_vec.push_back(j);

							//�������, �����ܲ�ͬ��, �����û�ʽ��ת, ��Ҫ��curr������; �˴���ѭ���� prev
							vector<double> tmpRcurr; //size=9
							double *pRprev = rtPrev.data() + 3;
							for(size_t axi=0; axi<3; axi++){
								double *pAxi = pRprev + axi * 3;
								for(size_t axj=0; axj<3; axj++){
									double *pAxj = pRcurr + axj * 3;
									double currPrevCos = dotProd(pAxi, pAxj, 3);
									if(abs(currPrevCos) > cos30){ //����ѭ������һ�� if true
										tmpRcurr.insert(tmpRcurr.end(), pAxj, pAxj + 3);
										if(currPrevCos < -cos30){
											for(size_t kk=0, idx=tmpRcurr.size()-1; kk<3; kk++, idx--)
												tmpRcurr[idx] *= -1;
										}
										break;
									}
								}
							}
							std::copy(tmpRcurr.begin(), tmpRcurr.end(), rt.begin()+3);

#if SINGLE_VERT
							break;
#else					//��Ҫbreak, �ɵ����Ƕ�λ, �ĳɶඥ�Ƕ�λ
							multiRmats_.insert(multiRmats_.end(), rt.begin()+3, rt.end());
							multiRmatsPrev_.insert(multiRmatsPrev_.end(), rtPrev.begin()+3, rtPrev.end());
							//break;
#endif //SINGLE_VERT

						}
					}
#if SINGLE_VERT
					if(foundMatch)
						break;
					//#else
#endif //SINGLE_VERT
				}
				//assert(foundMatch); //���� match, ˵���˶�����; ���ߴ�����bug, ��Ҫ����
				if(!foundMatch)
					return;

				if(poseIdxPrev_ != j_match){ //����ǰ֡��ǰһ֡��ƥ��ο�ϵ�����ǡ�ǰһ֡(�Լ���ʼ֡)���õĲο�ϵ //��, ������Щ
					//DEPRECATED...
				}

				//���delta(R,t): ���������ϵ, �����������
				//Affine3d cuPose, cuPosePrev, //�� (i-1), i ֡; �����Ĳο��ĸ�ֱ������ϵ, ֻ����ǰ���ǹ��ڡ�ͬһ��������ϵ
				//deltaPose; //��һ֡���֮ǰ��̬����

#if SINGLE_VERT
				Matrix3d cuRi_1, //R(i-1)
					cuRi; //R(i)
				Vector3d cuTi_1, //t(i-1)
					cuTi; //t(i)

				//��Ȼ��� R �Ѳ��öඥ��, ���� t ��Ȼ��ѡ��һ��(ѭ��β)�������ο�, �о�������, ��ʱ������ //2016-12-8 23:46:21
				vector<double> &rt = cubeCandiPoses[i_match];
				cuTi = Map<Vector3d>(rt.data());
				cuRi = Map<Matrix3d>(rt.data()+3);

				vector<double> &rtPrev = cubeCandiPosesPrev_[j_match];
				cuTi_1 = Map<Vector3d>(rtPrev.data());
				cuRi_1 = Map<Matrix3d>(rtPrev.data()+3);
#else //multi-vert //2016-12-14 16:40:32
				//������ t �ǡ�N�������ġ�, �� sum(verts)/N, ��Ҫ��ʼ�� 000
				Vector3d cuTi_1(0,0,0), //t(i-1)
					cuTi(0,0,0); //t(i)
				size_t nMatch = i_match_vec.size();
				for(size_t i=0; i<nMatch; i++){
					size_t indi = i_match_vec[i];
					size_t indj = j_match_vec[i]; //�൱������� i/j_match
					vector<double> &rt = cubeCandiPoses[indi];
					vector<double> &rtPrev = cubeCandiPosesPrev_[indj];
					cuTi += Vector3d(rt.data());
					cuTi_1 += Vector3d(rtPrev.data());
				}
				cuTi /= nMatch;
				cuTi_1 /= nMatch;
#endif //SINGLE_VERT

#if 0		//����, ��Ҫ��ǰ���ڴ���Ͼ��û���	//��ʵҲ���Դ˶��з������ڴ��, �ݲ�
				Matrix3d dR = cuRi_1 * cuRi.transpose(); //R(i-1)*Ri^(-1)
				//������ dRԼ����Eye, ��ʵ������Ϊ�������������, ���ܴ����û���, Ӧ�� Ri Ԥ������Ӧ��ת, ʹ��Լ���� R(i-1):
				dR = (dR.array() > 0.9).select(1, dR); //�ݶ�, ������ 0.9 ���� 1/0 ����ȫ�ʺ�; �����ϴ�ʱ dRӦֻ�� 1,0, Ϊ�û�����ת����
				dR = ((dR.array() >= -0.9) * (dR.array() <= 0.9)).select(0, dR);
				dR = (dR.array() < -0.9).select(-1, dR);

				cuRi = dR * cuRi;
				dR = cuRi_1 * cuRi.transpose();
#endif

#if PROCRUSTES //����֤, �� ���Ż������� ��� 3e-5m=0.03mm, ����ȫ��; ���˷�ʽ�� "�ඥ�ǹ�ͬ�Ż�" ��Ǳ�� //2016-10-3 01:21:15
				//tmp-test: ���� procrustes ����; ������� cubeCandiPoses �����������, û�������� //2016-10-3 00:12:28
				//�� R=argmin|R*A-B|, M=B*A.T; ���� A=i, B=i-1
#if SINGLE_VERT
				//Matrix3d M = cuRi * cuRi_1.transpose(); //��
				Matrix3d M = cuRi_1 * cuRi.transpose(); //��, ��ʱû��ϸ��
#else //!SINGLE_VERT //multi-vert �ඥ�Ƕ�λ
				assert(multiRmats_.size() != 0); //�˶��ڱز�Ϊ��
				//Matrix3Xd rmats = Map<Matrix3Xd>(multiRmats_.data()); //����: YOU_CALLED_A_FIXED_SIZE_METHOD_ON_A_DYNAMIC_SIZE_MATRIX_OR_VECTOR
				Matrix3Xd rmats = Map<Matrix3Xd>(multiRmats_.data(), 3, multiRmats_.size()/3); //3*3*m ��������3*3����m��
				Matrix3Xd rmatsPrev = Map<Matrix3Xd>(multiRmatsPrev_.data(), 3, multiRmatsPrev_.size()/3);
				Matrix3d M = rmatsPrev * rmats.transpose();

#endif //!SINGLE_VERT

				// 			JacobiSVD<Matrix3d> svd(Map<Matrix3d>(ax3orig.data()), ComputeFullU | ComputeFullV);
				JacobiSVD<Matrix3d> svd(M, ComputeFullU | ComputeFullV);
				Matrix3d svdU = svd.matrixU();
				Matrix3d svdV = svd.matrixV();
				Matrix3d dR = svdU * svdV.transpose(); //��������ת������, ��Ϊ cuRi �� cuRi1 ͬ��, ֻ������ת��׼
#endif //PROCRUSTES

				// #if !SINGLE_VERT //multi-vert �ඥ�Ƕ�λ
				// 			assert(multiRmats_.size() != 0); //�˶��ڱز�Ϊ��
				// 			//Matrix3Xd rmats = Map<Matrix3Xd>(multiRmats_.data()); //����: YOU_CALLED_A_FIXED_SIZE_METHOD_ON_A_DYNAMIC_SIZE_MATRIX_OR_VECTOR
				// 			Matrix3Xd rmats = Map<Matrix3Xd>(multiRmats_.data(), 3, multiRmats_.size()/3); //3*3*m ��������3*3����m��
				// 			/*Matrix3d */M = rmats * rmats.transpose();
				// 			//JacobiSVD<Matrix3d> svd2(M, ComputeFullU | ComputeFullV);
				// 			svd = JacobiSVD<Matrix3d>(M, ComputeFullU | ComputeFullV);
				// 			/*Matrix3d */svdU = svd.matrixU();
				// 			/*Matrix3d */svdV = svd.matrixV();
				// 			dR = svdU * svdV.transpose(); //��������ת������, ��Ϊ cuRi �� cuRi1 ͬ��, ֻ������ת��׼
				// #endif //!SINGLE_VERT

				Vector3d dT = -dR * cuTi + cuTi_1; //-dR*ti+t(i-1); ƽ�� tvec һֱ�����潻��, �����Ƿ���������

				//Affine3d &camPosePrev = camPoses_.back(); //camPoses_.back Ŀǰ��������Чȫ�� FLAG, ���Բ����������� //2016-12-9 00:03:17
				Affine3d &camPosePrev = camPoses_[lastValidIdx_];
				Matrix3d Ri1 = camPosePrev.linear();
				Vector3d ti1 = camPosePrev.translation();

				//dR���֮��, ���������̬;
				Affine3d camPoseCurr;
				camPoseCurr.linear() = Ri1 * dR; //Ri=R(i-1)*��Ri
				camPoseCurr.translation() = Ri1 * dT + ti1; //ti=R(i-1)*��ti + t(i-1)

				camPoses_.push_back(camPoseCurr);
			}//isFirstFrame_==false
			cubeCandiPosesPrev_ = cubeCandiPoses;
			lastValidIdx_ = camPoses_.size() - 1;
		}//isFindOrthoCorner==true

		//blend segmentation with rgb
		//cv::cvtColor(seg,seg,CV_RGB2BGR);
		//seg=(rgb+seg)/2.0;
		
		//show frame rate
		std::stringstream stext;
		stext<<"Frame Rate: "<<(1000.0/process_ms)<<"Hz";
		cv::putText(seg, stext.str(), cv::Point(15,15), cv::FONT_HERSHEY_SIMPLEX, 0.5, cv::Scalar(255,255,255,1));

		//zc: imshow ����UI���߳̿��ܵ�����������:
		//1. QCoreApplication::sendPostedEvents: Cannot send posted events for objects in another thread
		//2. QObject::moveToThread: Widgets cannot be moved to a new thread
		//3. QPixmap: It is not safe to use pixmaps outside the GUI thread
// 		cv::imshow("rgb", rgb);
// 		cv::imshow("seg", seg);
	}//onNewCloud

	//start the main loop
	void run ()
	{
		if(deviceId_ != ""){
			//pcl::Grabber* grabber = new pcl::OpenNIGrabber();
			//pcl::Grabber* grabber = new pcl::OpenNIGrabber("D:/Users/zhangxaochen/Documents/geo-cube399/00f80.oni");
			//pcl::Grabber* grabber = new pcl::OpenNIGrabber(deviceId_);
			grabber_ = new pcl::OpenNIGrabber(deviceId_);
			//grabber_->setDepthCameraIntrinsics(599.1f, 594.6f, 325.4, 252.1); //��Ч, ��ע����ȷ���� path, �� pcl180, ���� 160; ���Ƕ���ƽ�洹ֱ��ϵ���(��Ч)Ӱ�첻��, ���Ƿſ�ֱ�ж���ֵ
			// 		grabber_->setDepthCameraIntrinsics(579.267,585.016,311.056,242.254); //����ڲ�
			// 		grabber_->setDepthCameraIntrinsics(582.142,582.142,316.161,243.907); //Herrera�ڲ�
			grabber_->setDepthCameraIntrinsics(fx_, fy_, cx_, cy_);

			boost::function<void (const pcl::PointCloud<PtType>::ConstPtr&)> f =
				boost::bind (&MainLoop::onNewCloud, this, _1);

			grabber_->registerCallback(f);

			//grabbing loop
			grabber_->start();

			cv::namedWindow("rgb");
			cv::namedWindow("seg");
			cv::namedWindow("control", cv::WINDOW_NORMAL);

			int mergeMSETol=(int)pf.params.stdTol_merge,
				minSupport=(int)pf.minSupport,
				doRefine=(int)pf.doRefine;
			cv::createTrackbar("epsilon","control", &mergeMSETol, (int)pf.params.stdTol_merge*2);
			cv::createTrackbar("T_{NUM}","control", &minSupport, pf.minSupport*5);
			cv::createTrackbar("Refine On","control", &doRefine, 1);
			cv::createTrackbar("windowHeight","control", &pf.windowHeight, 2*pf.windowHeight);
			cv::createTrackbar("windowWidth","control", &pf.windowWidth, 2*pf.windowWidth);

			minSupport=0;

			//zc: play pf params
			pf.drawCoarseBorder = true;

			//GUI loop
			while (!done)
			{
				pf.params.stdTol_merge=(double)mergeMSETol;
				pf.minSupport=minSupport;
				pf.doRefine=doRefine!=0;

				//zc: imshow���»���ŵ����߳�
				cv::imshow("rgb", rgb);
				cv::imshow("seg", seg);

				//onKey(cv::waitKey(1000));
				onKey(cv::waitKey(this->pause_?0:1));
			}

			grabber_->stop();
		}

		if(0 != pngFnames_.size()){
			//����:
			cv::namedWindow("rgb");
			cv::namedWindow("seg");
			cv::namedWindow("control", cv::WINDOW_NORMAL);

			if(isQvga_){
				pf.minSupport /= (qvgaFactor_*qvgaFactor_);
				pf.windowHeight /= qvgaFactor_;
				pf.windowWidth /= qvgaFactor_;
			}

			int mergeMSETol=(int)pf.params.stdTol_merge,
				minSupport=(int)pf.minSupport,
				doRefine=(int)pf.doRefine;
			cv::createTrackbar("epsilon","control", &mergeMSETol, (int)pf.params.stdTol_merge*2);
			cv::createTrackbar("T_{NUM}","control", &minSupport, pf.minSupport*5);
			cv::createTrackbar("Refine On","control", &doRefine, 1);
			cv::createTrackbar("windowHeight","control", &pf.windowHeight, 2*pf.windowHeight);
			cv::createTrackbar("windowWidth","control", &pf.windowWidth, 2*pf.windowWidth);

			minSupport=0;
			pf.minSupport = minSupport; //��������

			//zc: play pf params
			pf.drawCoarseBorder = true;

			//cloudֻ����һ��:
			pcl::PointCloud<PtType>::Ptr pngCloud(new pcl::PointCloud<PtType>);
			pngCloud->is_dense = false; //false==�� nan
			pngCloud->width = 640 / qvgaFactor_;
			pngCloud->height = 480 / qvgaFactor_;
			//pngCloud->reserve(pngCloud->width * pngCloud->height); //Ԥ�����ڴ�, ��Ч: 75->50ms
			pngCloud->resize(pngCloud->width * pngCloud->height); //Ԥ�����ڴ�, ��Ч: 75->50ms

			//��ÿ�� dmap-png��
			for(size_t i=png_sid_; i<pngFnames_.size() && !this->done; i++){ //if done -> break
				string &fn = pngFnames_[i];
				printf("---------------png idx:= %d\n%s\n", i, fn.c_str());

				//mat->cloud
				Timer timer(1000);
				timer.tic();
				cv::Mat dmat = cv::imread(fn, cv::IMREAD_UNCHANGED); //������ 640*480
				dmat.convertTo(dmat, dmat.type(), dscale_);

				if(isQvga_)
					cv::pyrDown(dmat, dmat);

				timer.toctic("cv::imread: ");
				for(size_t iy=0; iy<dmat.rows; iy++){
					for(size_t ix=0; ix<dmat.cols; ix++){
						PtType pt;
						pt.r = pt.g = pt.b = 255; //��Ϊֻ���� depth-cloud, ���� rgb ���� fake
						ushort z = dmat.at<ushort>(iy, ix);
						if(z == 0){
							pt.x = pt.y = pt.z = std::numeric_limits<float>::quiet_NaN ();
						}
						else{
							float z_m = z * MM2M; //����->��(m); cloud ��׼��λ
							pt.x = (ix - cx_) / fx_ * z_m;
							pt.y = (iy - cy_) / fy_ * z_m;
							pt.z = z_m;
						}
						//pngCloud->points.push_back(pt); //reserve
						pngCloud->points[iy * dmat.cols + ix] = pt;
					}//for-ix
				}//for-iy
				timer.toc("mat->cloud:= ");

				this->onNewCloud(pngCloud);
				timer.toc("onNewCloud:= ");

				pf.params.stdTol_merge=(double)mergeMSETol;
				pf.minSupport=minSupport;
				pf.doRefine=doRefine!=0;

				//zc: imshow���»���ŵ����߳�
				cv::imshow("rgb", rgb);
				cv::imshow("seg", seg);

				//onKey(cv::waitKey(1000));
				onKey(cv::waitKey(this->pause_?0:1));
			}//for-pngFnames
			
			if(poseFn_ != "") //png ѭ��������, �������� "-sp", ��������̬�����ļ� csv 
				processPoses(poseFn_.c_str(), camPoses_);
		}

	}//MainLoop-run

	//handle keyboard commands
	void onKey(const unsigned char key)
	{
		static bool stop=false;
		switch(key) {
		case 'q': case 27:
			this->done=true;
			break;
		case ' ':
			this->pause_ = !this->pause_;
// 			if(pause_)
// 				this->grabber_->stop();
// 			else //pause_==false
// 				this->grabber_->start();
			break;
		case 's':
			this->pause_ = true;
// 			this->grabber_->stop();
			break;
		}
	}
};

//int main ()
int main (int argc, char* argv[])
{
	MainLoop loop;

	//�����õ� MainLoop ʵ����
	using namespace pcl::console;
	parse_argument(argc, argv, "-dev", deviceId_); //zc
	vector<double> depth_intrinsics;
	if(parse_x_arguments(argc, argv, "-intr", depth_intrinsics) > 0){
		fx_ = depth_intrinsics[0];
		fy_ = depth_intrinsics[1];
		cx_ = depth_intrinsics[2];
		cy_ = depth_intrinsics[3];
	}

	string png_dir;
	if(parse_argument(argc, argv, "-png_dir", png_dir) > 0){//��Ҫ�� dmap-png
		loop.pause_ = true; //���� png, Ĭ����ͣ
		pngFnames_ = getFnamesInDir(png_dir, ".png");
		if(0 == pngFnames_.size()){
			std::cout << "No PNG files found in folder: " << png_dir << std::endl;
			return -1;
		}
		std::sort(pngFnames_.begin(), pngFnames_.end());

		parse_argument(argc, argv, "-png_sid", png_sid_);
	}

	parse_argument(argc, argv, "-sp", poseFn_);
	//parse_argument(argc, argv, "-dbg1", dbg1_);
	dbg1_ = find_switch(argc, argv, "-dbg1");
	dbg2_ = find_switch(argc, argv, "-dbg2");
	if(dbg2_) //dbg2����dbg1
		dbg1_ = true;

	isQvga_ = find_switch(argc, argv, "-qvga");
	if(isQvga_){
		qvgaFactor_ = 2;
		fx_ /= qvgaFactor_;
		fy_ /= qvgaFactor_;
		cx_ /= qvgaFactor_;
		cy_ /= qvgaFactor_;
	}

	parse_argument(argc, argv, "-dscale", dscale_);

	loop.run();
	return 0;
}