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
    // existing
    int    gridW=40, gridH=30;
    double strip= 0.45;        // bottom band to learn ground
    double cellFrac= 0.55;     // vote inside a grid cell
    double centerCrop= 0.6;   // learn from center % of width
    double seedFrac= 0.10;     // bottom % of rows used as flood seeds
    double maxChroma= 0.35;    // chroma ceiling
    double yTolFrac= 0.55;     // luminance tolerance = max(12, yTolFrac*Yg)

    // new
    double topCut= 0.35;       // ignore top fraction when building grid (ceiling, lights)
    double specFrac = 0.20;     // treat (very bright && chroma < specFrac*tChrom) as specular → not free
    int    morphIters = 0;      // 3x3 open then close iterations on mask
    double seedWidth = 0.95;    // width of seed band (centered)
    double rowTaper = 0.20;     // tighten chroma threshold as we go up (0..1)
    int    minRun = 2;          // require this many contiguous free rows in each column near bottom
};

static void parseArgs(int argc,char**argv,Params&P){
    for(int i=2;i<argc;i++){
        std::string a=argv[i];
        auto nextI=[&](int&v){ if(i+1<argc) v=std::stoi(argv[++i]); };
        auto nextD=[&](double&v){ if(i+1<argc) v=std::stod(argv[++i]); };
        if(a=="--gridW") nextI(P.gridW);
        else if(a=="--gridH") nextI(P.gridH);
        else if(a=="--strip") nextD(P.strip);
        else if(a=="--cellFrac") nextD(P.cellFrac);
        else if(a=="--centerCrop") nextD(P.centerCrop);
        else if(a=="--seedFrac") nextD(P.seedFrac);
        else if(a=="--topCut") nextD(P.topCut);
        else if(a=="--specFrac") nextD(P.specFrac);
        else if(a=="--morph") nextI(P.morphIters);
        else if(a=="--seedWidth") nextD(P.seedWidth);
        else if(a=="--rowTaper") nextD(P.rowTaper);
        else if(a=="--minRun") nextI(P.minRun);
    }
}

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

int main(int argc,char**argv){
    if(argc<2){
        std::cerr<<"Usage: "<<argv[0]<<" input.jpg [--gridW 40] [--gridH 30] "
                   "[--strip 0.20] [--cellFrac 0.55] [-terCr-cenop 0.60] [--seedFrac 0.02]\n"
                   "       [--topCut 0.25] [--specFrac 0.40] [--morph 1] [--seedWidth 0.60]\n"
                   "       [--rowTaper 0.35] [--minRun 2]\n";
        return 1;
    }
    Params P; parseArgs(argc,argv,P);

    int w,h,n;
    unsigned char* data = stbi_load(argv[1], &w,&h,&n, 3);
    if(!data){ std::cerr<<"Failed to read "<<argv[1]<<"\n"; return 1; }
    std::vector<RGB> img(w*h);
    for(int i=0;i<w*h;i++){ img[i].r=data[3*i]; img[i].g=data[3*i+1]; img[i].b=data[3*i+2]; }
    stbi_image_free(data);

    // ---- Learn ground color & thresholds from bottom-center band
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

    // ---- Build initial mask with chroma + luminance
    std::vector<unsigned char> mask(w*h,0);
    for(int i=0;i<w*h;i++){
        const RGB& p=img[i];
        const int  Y = luminance(p);
        const bool chroma_ok = (chromaDist(p, ground) <= tChromBase);
        const bool y_ok = (std::abs(Y - Yg) <= yTol);
        mask[i] = (chroma_ok && y_ok) ? 255 : 0;
    }

    // ---- Suppress obvious specular highlights (very bright & very low chroma)
    if(P.specFrac>0){
        for(int i=0;i<w*h;i++){
            const RGB& p=img[i];
            if(luminance(p) > Yg + yTol + 12){
                if(chromaDist(p, ground) < P.specFrac * tChromBase) mask[i]=0;
            }
        }
    }

    // ---- Light morphology: open then close (speckle removal + small hole fill)
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

    // ---- Flood-fill reachability from tight bottom seeds (centered seedWidth)
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
int countMask = std::count(mask.begin(), mask.end(), 255);
int countReach = std::count(reach.begin(), reach.end(), 255);
std::cout << "Mask nonzero:  " << countMask  << "\n";
std::cout << "Reach nonzero: " << countReach << "\n";
std::cout << "Mask nonzero pixels: " << countMask << " / " << w*h << "\n";

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

    stbi_write_png("mask.png", w,h, 1, reach.data(), w);

// ---- Occupancy grid (near = low rows), ignore topCut
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

    // *** FIXED: no extra flip here ***
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

    // ---- Column sanity: require a small free run at the bottom of each column
    if(P.minRun>1){
        for(int gx=0; gx<P.gridW; ++gx){
            int run=0; bool ok=false;
            for(int gy=0; gy<std::min(P.gridH, 6); ++gy){ // check bottom ~20%
                if(grid[gy*P.gridW+gx]){ run++; if(run>=P.minRun){ ok=true; break; } }
                else run=0;
            }
            if(!ok){
                for(int gy=0; gy<P.gridH; ++gy) grid[gy*P.gridW+gx]=0;
            }
        }
    }

    // ---- Write files
    {
        std::ofstream csv("occupancy.csv");
        for(int gy=0; gy<P.gridH; ++gy){
            for(int gx=0; gx<P.gridW; ++gx){
                csv<<grid[gy*P.gridW+gx];
                if(gx+1<P.gridW) csv<<',';
            }
            csv<<'\n';
        }
    }
    {
        std::ofstream txt("occupancy.txt");
        txt<<"# occupancy grid (rows bottom->top). 1=available, 0=unavailable\n";
        for(int gy=0; gy<P.gridH; ++gy){
            for(int gx=0; gx<P.gridW; ++gx) txt<<grid[gy*P.gridW+gx];
            txt<<'\n';
        }
    }

    std::cout<<"Ground RGB "<<int(ground.r)<<","<<int(ground.g)<<","<<int(ground.b)
             <<"  Yg="<<Yg<<"  tChromBase="<<tChromBase<<"  yTol="<<yTol<<"\n";
    std::cout<<"mask.png, occupancy.csv, occupancy.txt written.\n";
    std::cout<<"Grid ("<<P.gridW<<"x"<<P.gridH<<"), rows near->far:\n";
    for(int gy=0; gy<P.gridH; ++gy){
        for(int gx=0; gx<P.gridW; ++gx) std::cout<<grid[gy*P.gridW+gx];
        std::cout<<'\n';
    }
    return 0;
}
