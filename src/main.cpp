#include "rv1106_tracker/config.hpp"
#include "rv1106_tracker/telemetry.hpp"
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace rv1106;
namespace {
void usage(const char* p) {
    std::cerr << "Usage: " << p << " --input <video|camera-index> [--roi x,y,w,h] [--config tracker.ini] [--mode accuracy|balanced|low-latency] [--headless]\n"
              << "Without --roi, select one rectangle with the mouse in the first frame.\n";
}
bool roi_arg(const std::string& text, cv::Rect2f& r) {
    return std::sscanf(text.c_str(), "%f,%f,%f,%f", &r.x,&r.y,&r.width,&r.height)==4;
}
}
int main(int argc,char** argv) {
    std::string input, config_path="tracker.ini", mode="balanced"; cv::Rect2f roi; bool have_roi=false, headless=false;
    for(int i=1;i<argc;++i) {
        std::string a=argv[i];
        if(a=="--input" && i+1<argc) input=argv[++i];
        else if(a=="--roi" && i+1<argc) have_roi=roi_arg(argv[++i],roi);
        else if(a=="--config" && i+1<argc) config_path=argv[++i];
        else if(a=="--mode" && i+1<argc) mode=argv[++i];
        else if(a=="--headless") headless=true;
        else { usage(argv[0]); return 2; }
    }
    if(input.empty()) { usage(argv[0]); return 2; }
    TrackerConfig cfg; try { cfg=load_config(config_path,cfg); } catch(const std::exception& e) {
        std::cerr << e.what() << " (using defaults if the file is absent is not implicit; pass an existing config)\n"; return 2;
    }
    if(mode=="accuracy") { cfg.search_scale_normal=3.0; cfg.search_scale_uncertain=4.5; cfg.flow_max_points=150; }
    else if(mode=="low-latency") { cfg.use_optical_flow=false; cfg.flow_max_points=25; cfg.frame_skip=std::max(cfg.frame_skip,1); }
    else if(mode!="balanced") { std::cerr << "Unknown mode: " << mode << '\n'; usage(argv[0]); return 2; }
    cv::VideoCapture cap;
    if(input.size()==1 && std::isdigit(input[0])) cap.open(input[0]-'0'); else cap.open(input);
    if(!cap.isOpened()) { std::cerr << "Unable to open input: " << input << '\n'; return 1; }
    cv::Mat frame; if(!cap.read(frame) || frame.empty()) { std::cerr << "No first frame\n"; return 1; }
    if(!have_roi) {
        if(headless) { std::cerr << "--roi is required in headless mode\n"; return 2; }
        cv::namedWindow("Select target ROI",cv::WINDOW_NORMAL);
        cv::Rect selected=cv::selectROI("Select target ROI",frame,false,false); cv::destroyWindow("Select target ROI");
        roi=cv::Rect2f(selected); have_roi=selected.width>0 && selected.height>0;
    }
    SingleObjectTracker tracker(cfg);
    if(!have_roi || !tracker.initialize(frame,roi)) { std::cerr << "Invalid target ROI\n"; return 2; }
    uint64_t frame_id=0;
    do {
        const auto now=std::chrono::steady_clock::now().time_since_epoch();
        const auto ts=static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        TrackingResult result=tracker.update(frame,frame_id++,ts);
        std::cout << to_json(result,frame.cols,frame.rows) << '\n';
        if(!headless) {
            cv::Mat display=frame.clone(); const cv::Scalar color=result.state==TrackingState::Tracking?cv::Scalar(40,220,40):
                (result.state==TrackingState::Weak?cv::Scalar(0,190,255):cv::Scalar(30,30,230));
            cv::rectangle(display,result.box,color,2);
            cv::circle(display,result.center,3,color,-1);
            const std::string label=std::string(to_string(result.state))+" conf="+std::to_string(result.confidence)+
                " "+std::to_string(result.latency_ms)+"ms";
            cv::putText(display,label,{12,26},cv::FONT_HERSHEY_SIMPLEX,.65,color,2);
            cv::imshow("RV1106 visual tracker",display);
            if(cv::waitKey(1)==27) break;
        }
        for(int n=0;n<cfg.frame_skip;++n) if(!cap.read(frame)) return 0;
    } while(cap.read(frame) && !frame.empty());
    return 0;
}
