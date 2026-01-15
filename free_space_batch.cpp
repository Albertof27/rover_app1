// free_space_batch.cpp
// Build with: C++17 (VS: Project Properties -> C/C++ -> Language -> C++ Language Standard -> ISO C++17)

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

struct RGB { unsigned char r,g,b; };

inline double chromaDist(const RGB& a, const RGB& b){
    const double sa = std::max(1.0, double(a.r)+a.g+a.b);
    const double sb = std::max(1.0, double(b.r)+b.g+b.b);
    const double ar=a.r/sa, ag=a.g/sa, br=b.r/sb, bg=b.g/sb;
    const double dr=ar-br, dg=ag-bg;
    return std::sqrt(dr*dr+dg*dg);
}
inline int luminance(const RGB& p){ return int(0.2126*p.r + 0.7152*p.g + 0.0722*p.b); }

struct Params{
    int    gridW=40, gridH=30;
    double strip= 0.45;
    double cellFrac= 0.55;
    double centerCrop= 0.60;
    double seedFrac= 0.10;
    double maxChroma= 0.35;
    double yTolFrac= 0.55;

    double topCut= 0.35;
    double specFrac = 0.20;
    int    morphIters = 0;
    double seedWidth = 0.95;
    double rowTaper = 0.20;
    int    minRun = 2;
};

static unsigned char kthMedian(std::vector<int>& v){
    if(v.empty()) return 128;
    size_t k=v.size()/2;
    std::nth_element(v.begin(), v.begin()+k, v.end());
    return (unsigned char)v[k];
}
static double percentile(std::vector<double>&v,double p){
    if(v.empty()) return 0.10;
    p=std::clamp(p,0.0,1.0);
    size_t k=std::clamp<size_t>(size_t(std::round(p*(v.size()-1))),0,v.size()-1);
    std::nth_element(v.begin(), v.begin()+k, v.end());
    return v[k];
}

// 3x3 binary morphology helpers on a 8-bit mask (0/255)
static void binErode(const std::vector<unsigned char>& src, int w, int h,
                     std::vector<unsigned char>& dst){
    dst.assign(w*h,0);
    for(int y=1;y<h-1;++y){
        for(int x=1;x<w-1;++x){
            int all=255;
            for(int yy=y-1;yy<=y+1;++yy)
                for(int xx=x-1;xx<=x+1;++xx)
                    all = (all && src[yy*w+xx]==255) ? 255 : 0;
            dst[y*w+x] = all;
        }
    }
}
static void binDilate(const std::vector<unsigned char>& src, int w, int h,
                      std::vector<unsigned char>& dst){
    dst.assign(w*h,0);
    for(int y=1;y<h-1;++y){
        for(int x=1;x<w-1;++x){
            int any=0;
            for(int yy=y-1;yy<=y+1;++yy)
                for(int xx=x-1;xx<=x+1;++xx)
                    any = (any || src[yy*w+xx]==255) ? 255 : 0;
            dst[y*w+x] = any;
        }
    }
}

struct CLI {
    std::vector<std::string> inputs; // files or directories
    std::string outDir = ".";
    bool recurse = false;
    Params P;
};

static bool isImageExt(const fs::path& p){
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return (e==".jpg" || e==".jpeg" || e==".png" || e==".bmp" || e==".tga");
}

static void parseArgs(int argc, char** argv, CLI& cli){
    for(int i=1;i<argc;++i){
        std::string a = argv[i];
        auto nextI=[&](int&v){ if(i+1<argc) v=std::stoi(argv[++i]); };
        auto nextD=[&](double&v){ if(i+1<argc) v=std::stod(argv[++i]); };
        auto nextS=[&](std::string&v){ if(i+1<argc) v=argv[++i]; };

        if(a=="--gridW") nextI(cli.P.gridW);
        else if(a=="--gridH") nextI(cli.P.gridH);
        else if(a=="--strip") nextD(cli.P.strip);
        else if(a=="--cellFrac") nextD(cli.P.cellFrac);
        else if(a=="--centerCrop") nextD(cli.P.centerCrop);
        else if(a=="--seedFrac") nextD(cli.P.seedFrac);
        else if(a=="--topCut") nextD(cli.P.topCut);
        else if(a=="--specFrac") nextD(cli.P.specFrac);
        else if(a=="--morph") nextI(cli.P.morphIters);
        else if(a=="--seedWidth") nextD(cli.P.seedWidth);
        else if(a=="--rowTaper") nextD(cli.P.rowTaper);
        else if(a=="--minRun") nextI(cli.P.minRun);
        else if(a=="--out") nextS(cli.outDir);
        else if(a=="--recurse") cli.recurse = true;
        else if(!a.empty() && a[0]=='-'){
            std::cerr << "Unknown option: " << a << "\n";
        } else {
            cli.inputs.push_back(a);
        }
    }
}

static bool ensureDir(const fs::path& p){
    std::error_code ec;
    if(p.empty()) return true;
    if(fs::exists(p, ec)){
        return fs::is_directory(p, ec);
    }
    return fs::create_directories(p, ec);
}

// === Core processing for a single image (mostly your original code) ===
static bool process_one_image(const fs::path& inPath, const fs::path& outDir, const Params& P){
    int w,h,n;
    std::string inStr = inPath.string();
    unsigned char* data = stbi_load(inStr.c_str(), &w,&h,&n, 3);
    if(!data){
        std::cerr<<"[SKIP] Failed to read "<< inStr <<"\n";
        return false;
    }
    std::vector<RGB> img(w*h);
    for(int i=0;i<w*h;i++){ img[i].r=data[3*i]; img[i].g=data[3*i+1]; img[i].b=data[3*i+2]; }
    stbi_image_free(data);

    // Learn ground color & thresholds from bottom-center band
    const int y0 = std::max(0, h - int(std::round(P.strip*h)));
    const double side = (1.0-std::clamp(P.centerCrop,0.2,1.0))*0.5;
    const int xL=int(std::round(side*w)), xR=int(std::round((1.0-side)*w));

    std::vector<int> Rs,Gs,Bs;
    std::vector<double> Ds; Ds.reserve((xR-xL)*(h-y0));
    Rs.reserve(Ds.capacity()); Gs.reserve(Ds.capacity()); Bs.reserve(Ds.capacity());

    for(int y=y0;y<h;++y)
        for(int x=xL;x<xR;++x){
            const RGB& p=img[y*w+x];
            Rs.push_back(p.r); Gs.push_back(p.g); Bs.push_back(p.b);
        }
    RGB ground{ kthMedian(Rs), kthMedian(Gs), kthMedian(Bs) };
    const int Yg = luminance(ground);
    const int yTol = std::max(12, int(P.yTolFrac*Yg));

    for(int y=y0;y<h;++y)
        for(int x=xL;x<xR;++x){
            const RGB& p=img[y*w+x];
            if(std::abs(luminance(p)-Yg) <= yTol)
                Ds.push_back(chromaDist(p, ground));
        }

    double tChromBase = std::min(P.maxChroma, percentile(Ds, 0.90));

    // Initial mask
    std::vector<unsigned char> mask(w*h,0);
    for(int i=0;i<w*h;i++){
        const RGB& p=img[i];
        const int  Y = luminance(p);
        const bool chroma_ok = (chromaDist(p, ground) <= tChromBase);
        const bool y_ok = (std::abs(Y - Yg) <= yTol);
        mask[i] = (chroma_ok && y_ok) ? 255 : 0;
    }

    // Specular suppression
    if(P.specFrac>0){
        for(int i=0;i<w*h;i++){
            const RGB& p=img[i];
            if(luminance(p) > Yg + yTol + 12){
                if(chromaDist(p, ground) < P.specFrac * tChromBase) mask[i]=0;
            }
        }
    }

    // Morphology
    if(P.morphIters>0){
        std::vector<unsigned char> tmp(mask.size());
        for(int it=0; it<P.morphIters; ++it){
            binErode(mask,w,h,tmp);
            binDilate(tmp,w,h,mask);
        }
        for(int it=0; it<P.morphIters; ++it){
            binDilate(mask,w,h,tmp);
            binErode(tmp,w,h,mask);
        }
    }

    // Flood fill reachability
    std::vector<unsigned char> reach(w*h,0);
    std::queue<std::pair<int,int>> q;
    const int seedRows = std::max(1, int(std::round(P.seedFrac*h)));
    const int sy0 = std::max(0, h-seedRows);
    const double sw = std::clamp(P.seedWidth,0.2,1.0);
    const int sL = int((0.5 - 0.5*sw) * w);
    const int sR = int((0.5 + 0.5*sw) * w);
    for(int y=sy0;y<h;++y)
        for(int x=sL;x<sR;++x)
            if(mask[y*w+x]){ q.push({x,y}); reach[y*w+x]=255; }

    auto inB=[&](int x,int y){ return (x>=0&&y>=0&&x<w&&y<h); };
    const int dx4[4]={1,-1,0,0}, dy4[4]={0,0,1,-1};
    while(!q.empty()){
        auto [x,y]=q.front(); q.pop();
        for(int k=0;k<4;k++){
            int nx=x+dx4[k], ny=y+dy4[k];
            if(!inB(nx,ny)) continue;
            int idx=ny*w+nx;
            if(!mask[idx] || reach[idx]) continue;
            reach[idx]=255; q.push({nx,ny});
        }
    }

    // Occupancy grid (near = low rows), ignore topCut
    const int useH   = std::max(1, int((1.0 - std::clamp(P.topCut,0.0,0.9)) * h));
    const int yUseTop = h - useH;

    std::vector<int> grid(P.gridH*P.gridW,0);
    for (int gy=0; gy<P.gridH; ++gy) {
        // invert gy so gy=0 is near/bottom in image coords
        const int gy_inv = P.gridH - 1 - gy;

        const double upFrac  = (double)gy / std::max(1, P.gridH-1);
        const double tChrom  = tChromBase * (1.0 - P.rowTaper*upFrac);

        int im_y_start = int(std::floor((double)gy_inv     / P.gridH * useH)) + yUseTop;
        int im_y_end   = int(std::floor((double)(gy_inv+1) / P.gridH * useH)) + yUseTop;
        im_y_end = std::max(im_y_end, im_y_start+1);

        int y_top = std::clamp(im_y_start, 0, h);
        int y_bot = std::clamp(im_y_end,   0, h);

        for (int gx=0; gx<P.gridW; ++gx) {
            int im_x_start = int(std::floor((double)gx     / P.gridW * w));
            int im_x_end   = int(std::floor((double)(gx+1) / P.gridW * w));
            im_x_end = std::max(im_x_end, im_x_start+1);

            long long freeCount=0, pixCount=0;
            for (int yy=y_top; yy<y_bot; ++yy) {
                for (int xx=im_x_start; xx<im_x_end; ++xx) {
                    ++pixCount;
                    if (reach[yy*w+xx]) { ++freeCount; continue; }
                    const RGB& p = img[yy*w+xx];
                    if (std::abs(luminance(p)-Yg) <= yTol &&
                        chromaDist(p,ground) <= tChrom) ++freeCount;
                }
            }
            double frac = pixCount ? (double)freeCount/pixCount : 0.0;
            grid[gy*P.gridW+gx] = (frac >= P.cellFrac) ? 1 : 0;
        }
    }

    // Output names
    fs::path stem = inPath.stem();
    fs::path maskPath = outDir / fs::path("mask_" + stem.string() + ".png");
    fs::path csvPath  = outDir / fs::path("occupancy_" + stem.string() + ".csv");
    fs::path txtPath  = outDir / fs::path("occupancy_" + stem.string() + ".txt");

    // Write mask image (reachability)
    stbi_write_png(maskPath.string().c_str(), w,h, 1, reach.data(), w);

    // Write occupancy CSV
    {
        std::ofstream csv(csvPath);
        for(int gy=0; gy<P.gridH; ++gy){
            for(int gx=0; gx<P.gridW; ++gx){
                csv<<grid[gy*P.gridW+gx];
                if(gx+1<P.gridW) csv<<',';
            }
            csv<<'\n';
        }
    }
    // Write occupancy TXT
    {
        std::ofstream txt(txtPath);
        txt<<"# occupancy grid (rows bottom->top). 1=available, 0=unavailable\n";
        for(int gy=0; gy<P.gridH; ++gy){
            for(int gx=0; gx<P.gridW; ++gx) txt<<grid[gy*P.gridW+gx];
            txt<<'\n';
        }
        txt << "Ground RGB " << int(ground.r) << "," << int(ground.g) << "," << int(ground.b)
            << "  Yg=" << Yg << "  tChromBase=" << tChromBase << "  yTol=" << yTol << "\n";
    }

    std::cout<<"[OK] "<< inStr <<"\n"
             <<"     -> "<< maskPath.string() <<"\n"
             <<"     -> "<< csvPath.string()  <<"\n"
             <<"     -> "<< txtPath.string()  <<"\n";
    return true;
}

int main(int argc,char**argv){
    CLI cli;
    parseArgs(argc, argv, cli);

    if(cli.inputs.empty()){
        std::cerr <<
        "Usage: " << argv[0] << " [images_or_dirs ...] [--out <dir>] [--recurse]\n"
        "       [--gridW 40] [--gridH 30] [--strip 0.45] [--cellFrac 0.55] [--centerCrop 0.60]\n"
        "       [--seedFrac 0.10] [--topCut 0.35] [--specFrac 0.20] [--morph 0]\n"
        "       [--seedWidth 0.95] [--rowTaper 0.20] [--minRun 2]\n";
        return 1;
    }

    if(!ensureDir(cli.outDir)){
        std::cerr << "Failed to create/access output directory: " << cli.outDir << "\n";
        return 1;
    }
    fs::path outP(cli.outDir);

    // Collect images
    std::vector<fs::path> images;
    for(const auto& in : cli.inputs){
        fs::path p(in);
        std::error_code ec;
        if(fs::is_regular_file(p, ec) && isImageExt(p)){
            images.push_back(fs::canonical(p, ec).empty() ? p : fs::canonical(p, ec));
        }else if(fs::is_directory(p, ec)){
            if(cli.recurse){
                for(auto& e : fs::recursive_directory_iterator(p, ec)){
                    if(!ec && e.is_regular_file() && isImageExt(e.path()))
                        images.push_back(e.path());
                }
            }else{
                for(auto& e : fs::directory_iterator(p, ec)){
                    if(!ec && e.is_regular_file() && isImageExt(e.path()))
                        images.push_back(e.path());
                }
            }
        }else{
            std::cerr << "[WARN] Not a valid file/dir or unsupported type: " << in << "\n";
        }
    }

    if(images.empty()){
        std::cerr << "No images found. Supported extensions: .jpg .jpeg .png .bmp .tga\n";
        return 1;
    }

    size_t ok=0, fail=0;
    for(const auto& img : images){
        if(process_one_image(img, outP, cli.P)) ++ok; else ++fail;
    }
    std::cout << "\nDone. Success: " << ok << "  Failed: " << fail << "\n";
    return (fail==0)? 0 : 2;
}
