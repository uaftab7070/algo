#include "rv1106_tracker/config.hpp"
#include "rv1106_tracker/tracker.hpp"
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
struct Gt { int frame; cv::Rect2f box; };
float iou(const cv::Rect2f& a,const cv::Rect2f& b) {
    const float inter=(a&b).area(); const float u=a.area()+b.area()-inter; return u>0?inter/u:0;
}
}
int main(int argc,char** argv) {
    if(argc<7) { std::cerr<<"Usage: "<<argv[0]<<" video initial_x initial_y initial_w initial_h ground_truth.csv [config.ini]\n"; return 2; }
    const std::string video=argv[1]; cv::Rect2f roi(std::stof(argv[2]),std::stof(argv[3]),std::stof(argv[4]),std::stof(argv[5]));
    std::ifstream gtfile(argv[6]); if(!gtfile){std::cerr<<"Cannot open ground truth CSV\n";return 2;}
    std::vector<Gt> truth; std::string line; while(std::getline(gtfile,line)) {
        if(line.empty()||line[0]=='#') continue; std::replace(line.begin(),line.end(),',',' ');
        std::istringstream s(line); Gt g; if(s>>g.frame>>g.box.x>>g.box.y>>g.box.width>>g.box.height) truth.push_back(g);
    }
    if(truth.empty()){std::cerr<<"Ground truth CSV is empty\n";return 2;}
    rv1106::TrackerConfig cfg; if(argc>7) cfg=rv1106::load_config(argv[7],cfg);
    cv::VideoCapture cap(video); cv::Mat frame; if(!cap.read(frame)){std::cerr<<"Cannot read video\n";return 1;}
    rv1106::SingleObjectTracker tracker(cfg); if(!tracker.initialize(frame,roi)){std::cerr<<"Invalid ROI\n";return 2;}
    size_t idx=0; double iou_sum=0,center_sum=0; int tracked=0,lost=0,false_track=0; double elapsed=0;
    for(int f=0; !frame.empty(); ++f) {
        auto t=cv::getTickCount(); auto r=tracker.update(frame,f,0); elapsed+=(cv::getTickCount()-t)/cv::getTickFrequency()*1000.0;
        if(idx<truth.size() && truth[idx].frame==f) {
            const float overlap=iou(r.box,truth[idx].box); iou_sum+=overlap;
            const cv::Point2f c1(r.center),c2(truth[idx].box.x+truth[idx].box.width*.5F,truth[idx].box.y+truth[idx].box.height*.5F);
            center_sum+=cv::norm(c1-c2); ++tracked;
            if(r.state==rv1106::TrackingState::Lost) ++lost;
            if(r.state!=rv1106::TrackingState::Lost && overlap<.1F) ++false_track;
            ++idx;
        }
        if(!cap.read(frame)) break;
    }
    std::cout<<"frames="<<tracked<<" mean_iou="<<(tracked?iou_sum/tracked:0)
             <<" mean_center_error_px="<<(tracked?center_sum/tracked:0)<<" lost_frames="<<lost
             <<" false_track_frames_iou_lt_0.1="<<false_track<<" avg_tracker_latency_ms="<<(tracked?elapsed/tracked:0)<<'\n';
    return 0;
}
