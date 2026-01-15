// free_space_watch.cpp
// Build: g++ free_space_watch.cpp -std=c++17 -O2 -o free_space_watch.exe
// Needs stb_image.h and stb_image_write.h in the same folder.

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

struct RGB { unsigned char r,g,b; };

// === Helpers ================================================================

inline double chromaDist(const RGB& a, const RGB& b){
    const double sa = std::max(1.0, double(a.r)+a.g+a.b);
    const double sb = std::max(1.0, double(b.r)+b.g+b.b);
    const double ar=a.r/sa, ag=a.g/sa, br=b.r/sb, bg=b.g/sb;
    const double dr=ar-br, dg=ag-bg;
    return std::sqrt(dr*dr+dg*dg);
}
inline int luminance(const RGB& p){ return int(0.2126*p.r + 0.7152*p.g + 0.0722*p.b); }

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

// 3x3 binary morphology
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

// Simple float3 and conversions
struct F3 { float r,g,b; };
static inline F3  toF (const RGB& p){ return F3{ float(p.r), float(p.g), float(p.b) }; }
static inline RGB toU8(const F3& f){
    auto cu8=[&](float x){ x = std::round(std::clamp(x,0.0f,255.0f)); return (unsigned char)x; };
    return RGB{ cu8(f.r), cu8(f.g), cu8(f.b) };
}

// Bilinear resize
static std::vector<RGB> resizeBilinear(const std::vector<RGB>& src, int w, int h, int W, int H){
    std::vector<RGB> dst(W*H);
    const float sx = (float)w / W, sy = (float)h / H;
    for(int y=0;y<H;++y){
        float fy = (y + 0.5f)*sy - 0.5f;
        int y0 = (int)std::floor(fy), y1 = std::min(h-1, y0+1); float wy = fy - y0;
        y0 = std::clamp(y0, 0, h-1);
        for(int x=0;x<W;++x){
            float fx = (x + 0.5f)*sx - 0.5f;
            int x0 = (int)std::floor(fx), x1 = std::min(w-1, x0+1); float wx = fx - x0;
            x0 = std::clamp(x0, 0, w-1);

            auto s00 = toF(src[y0*w+x0]), s10 = toF(src[y0*w+x1]);
            auto s01 = toF(src[y1*w+x0]), s11 = toF(src[y1*w+x1]);

            F3 a{
                (1-wx)*((1-wy)*s00.r + wy*s01.r) + wx*((1-wy)*s10.r + wy*s11.r),
                (1-wx)*((1-wy)*s00.g + wy*s01.g) + wx*((1-wy)*s10.g + wy*s11.g),
                (1-wx)*((1-wy)*s00.b + wy*s01.b) + wx*((1-wy)*s10.b + wy*s11.b)
            };
            dst[y*W+x] = toU8(a);
        }
    }
    return dst;
}

// Box blur (separable) for unsharp
static void boxBlurChannel(const unsigned char* src, unsigned char* dst, int w, int h, int r){
    std::vector<int> tmp(w*h);
    int dim = 2*r + 1;

    // Horizontal
    for(int y=0;y<h;++y){
        int sum=0;
        for(int i=-r;i<=r;++i) sum += src[y*w + std::clamp(i,0,w-1)];
        for(int x=0;x<w;++x){
            tmp[y*w+x] = sum;
            int xout = x - r;
            int xin  = x + r + 1;
            sum -= src[y*w + std::clamp(xout,0,w-1)];
            sum += src[y*w + std::clamp(xin ,0,w-1)];
        }
    }
    // Vertical
    for(int x=0;x<w;++x){
        int sum=0;
        for(int i=-r;i<=r;++i) sum += tmp[std::clamp(i,0,h-1)*w + x];
        for(int y=0;y<h;++y){
            dst[y*w + x] = (unsigned char) std::clamp(sum / (dim*dim), 0, 255);
            int yout = y - r;
            int yin  = y + r + 1;
            sum -= tmp[std::clamp(yout,0,h-1)*w + x];
            sum += tmp[std::clamp(yin ,0,h-1)*w + x];
        }
    }
}

// Unsharp (with optional Laplacian boost)
static void unsharp(std::vector<RGB>& img, int w, int h, int radius, double amount, double laplaceBoost){
    if(amount<=0.0 || radius<1) return;

    std::vector<unsigned char> r(w*h), g(w*h), b(w*h), rb(w*h), gb(w*h), bb(w*h);
    for(int i=0;i<w*h;++i){ r[i]=img[i].r; g[i]=img[i].g; b[i]=img[i].b; }

    boxBlurChannel(r.data(), rb.data(), w,h, radius);
    boxBlurChannel(g.data(), gb.data(), w,h, radius);
    boxBlurChannel(b.data(), bb.data(), w,h, radius);

    auto lap = [&](int idx)->F3{
        if(laplaceBoost<=0.0) return F3{0,0,0};
        int y = idx / w, x = idx % w;
        auto at=[&](int xx,int yy)->RGB{
            xx = std::clamp(xx,0,w-1); yy = std::clamp(yy,0,h-1);
            return img[yy*w+xx];
        };
        int c = at(x,y).r*4 - at(x-1,y).r - at(x+1,y).r - at(x,y-1).r - at(x,y+1).r;
        int d = at(x,y).g*4 - at(x-1,y).g - at(x+1,y).g - at(x,y-1).g - at(x,y+1).g;
        int e = at(x,y).b*4 - at(x-1,y).b - at(x+1,y).b - at(x,y-1).b - at(x,y+1).b;
        return F3{ (float)c, (float)d, (float)e };
    };

    for(int i=0;i<w*h;++i){
        float rr = (float)r[i] + (float)amount * ((float)r[i] - (float)rb[i]);
        float gg = (float)g[i] + (float)amount * ((float)g[i] - (float)gb[i]);
        float bbv= (float)b[i] + (float)amount * ((float)b[i] - (float)bb[i]);
        if(laplaceBoost>0.0){
            auto L = lap(i);
            rr += (float)laplaceBoost * L.r;
            gg += (float)laplaceBoost * L.g;
            bbv+= (float)laplaceBoost * L.b;
        }
        img[i] = toU8(F3{rr,gg,bbv});
    }
}

// === Parameters & CLI =======================================================

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

struct CLI {
    std::vector<std::string> inputs;
    std::string outDir = ".";
    bool recurse = false;
    bool watch = false;
    int  interval_ms = 1000;
    double sharpen = 0.0;
    int    radius  = 2;
    double laplaceBoost = 0.0;
    double scale   = 1.0;
    bool   dumpPre = false;
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
        else if(a=="--watch")   cli.watch = true;
        else if(a=="--interval"){ int v; nextI(v); cli.interval_ms = std::max(1, v); }
        else if(a=="--sharpen"){ double v; nextD(v); cli.sharpen = std::max(0.0, v); }
        else if(a=="--radius"){ int v; nextI(v); cli.radius = std::max(1, v); }
        else if(a=="--laplaceBoost"){ double v; nextD(v); cli.laplaceBoost = std::max(0.0, v); }
        else if(a=="--scale"){ double v; nextD(v); cli.scale = std::max(0.1, v); }
        else if(a=="--dumpPre") cli.dumpPre = true;
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
    if(fs::exists(p, ec)) return fs::is_directory(p, ec);
    return fs::create_directories(p, ec);
}

// === Core processing ========================================================

static bool process_one_image(const fs::path& inPath, const fs::path& outDir, const Params& P,
                              double scale, double sharpenAmt, int radius,
                              double laplaceBoost, bool dumpPre)
{
    int w,h,n;
    unsigned char* data = stbi_load(inPath.string().c_str(), &w,&h,&n, 3);
    if(!data){
        std::cerr<<"[SKIP] Failed to read "<< inPath.string() <<"\n";
        return false;
    }
    std::vector<RGB> img(w*h);
    for(int i=0;i<w*h;i++){ img[i].r=data[3*i]; img[i].g=data[3*i+1]; img[i].b=data[3*i+2]; }
    stbi_image_free(data);

    if(scale != 1.0){
        int W = std::max(1, int(std::round(w*scale)));
        int H = std::max(1, int(std::round(h*scale)));
        img = resizeBilinear(img, w,h, W,H);
        w=W; h=H;
    }
    if(sharpenAmt > 0.0){
        unsharp(img, w,h, radius, sharpenAmt, laplaceBoost);
    }
    if(dumpPre){
        fs::path pre = outDir / fs::path("pre_" + inPath.stem().string() + ".jpg");
        stbi_write_jpg(pre.string().c_str(), w,h, 3, img.data(), 90);
    }

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

    // Morphology (open then close)
    if(P.morphIters>0){
        std::vector<unsigned char> tmp(mask.size());
        for(int it=0; it<P.morphIters; ++it){ binErode(mask,w,h,tmp); binDilate(tmp,w,h,mask); }
        for(int it=0; it<P.morphIters; ++it){ binDilate(mask,w,h,tmp); binErode(tmp,w,h,mask); }
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

    // Occupancy grid
    const int useH   = std::max(1, int((1.0 - std::clamp(P.topCut,0.0,0.9)) * h));
    const int yUseTop = h - useH;

    std::vector<int> grid(P.gridH*P.gridW,0);
    for (int gy=0; gy<P.gridH; ++gy) {
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

    fs::path stem = inPath.stem();
    fs::path maskPath = outDir / fs::path("mask_" + stem.string() + ".png");
    fs::path csvPath  = outDir / fs::path("occupancy_" + stem.string() + ".csv");
    fs::path txtPath  = outDir / fs::path("occupancy_" + stem.string() + ".txt");

    stbi_write_png(maskPath.string().c_str(), w,h, 1, reach.data(), w);

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
    {
        std::ofstream txt(txtPath);
        txt<<"# occupancy grid (rows bottom->top). 1=available, 0=unavailable\n";
        for(int gy=0; gy<P.gridH; ++gy){
            for(int gx=0; gx<P.gridW; ++gx) txt<<grid[gy*P.gridW+gx];
            txt<<'\n';
        }
    }

    std::cout << "[OK] " << inPath.string() << "\n";
    return true;
}

// === Watcher ================================================================

static void collect_images_in(const fs::path& root, bool recurse, std::vector<fs::path>& out){
    std::error_code ec;
    if(recurse){
        for(auto& e : fs::recursive_directory_iterator(root, ec)){
            if(!ec && e.is_regular_file() && isImageExt(e.path()))
                out.push_back(e.path());
        }
    }else{
        for(auto& e : fs::directory_iterator(root, ec)){
            if(!ec && e.is_regular_file() && isImageExt(e.path()))
                out.push_back(e.path());
        }
    }
}

static std::atomic<bool> g_stop{false};
static void on_sigint(int){ g_stop = true; }

// === Main ===================================================================

int main(int argc, char** argv){
    std::signal(SIGINT, on_sigint); // Ctrl+C

    CLI cli; parseArgs(argc, argv, cli);

    if(cli.inputs.empty()){
        std::cerr <<
        "Usage: " << argv[0] << " [files_or_dirs ...] [--out <dir>] [--recurse] [--watch] [--interval ms]\n"
        "Tuning: --gridW --gridH --strip --cellFrac --centerCrop --seedFrac --topCut --specFrac --morph --seedWidth --rowTaper --minRun\n"
        "Quality: --scale <f> --sharpen <f> --radius <i> --laplaceBoost <f> --dumpPre\n";
        return 1;
    }

    if(!ensureDir(cli.outDir)){
        std::cerr << "Failed to create/access output directory: " << cli.outDir << "\n";
        return 1;
    }
    fs::path outP(cli.outDir);

    // Track processed files by absolute path + size + write ticks
    struct Key {
        fs::path p;
        uintmax_t sz;
        long long wt;
        bool operator==(const Key& o) const noexcept { return p==o.p && sz==o.sz && wt==o.wt; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const noexcept {
            return std::hash<std::string>()(k.p.string())
                   ^ (std::hash<uintmax_t>()(k.sz)<<1)
                   ^ (std::hash<long long>()(k.wt)<<2);
        }
    };
    std::unordered_set<Key,KeyHash> seen;

    auto scan_once = [&](size_t& ok, size_t& fail){
        std::vector<fs::path> batch;
        for(const auto& in : cli.inputs){
            std::error_code ec;
            fs::path p(in);
            if(fs::is_regular_file(p, ec) && isImageExt(p)){
                batch.push_back(p);
            } else if(fs::is_directory(p, ec)){
                collect_images_in(p, cli.recurse, batch);
            }
        }

        for(const auto& img : batch){
            std::error_code ec;
            auto abs   = fs::absolute(img, ec);
            auto fsz   = fs::file_size(img, ec);
            auto wtime = fs::last_write_time(img, ec);
            if(ec) continue;

            auto wticks = wtime.time_since_epoch().count(); // C++17-safe
            Key key{abs, fsz, static_cast<long long>(wticks)};
            if(seen.find(key) != seen.end()) continue;

            // If watching, ensure file settled
            if(cli.watch){
                auto s1 = fsz;
                std::this_thread::sleep_for(std::chrono::milliseconds(std::max(50, cli.interval_ms/4)));
                auto s2 = fs::file_size(img, ec);
                if(!ec && s2!=s1) continue;
            }

            if(process_one_image(img, outP, cli.P, cli.scale, cli.sharpen, cli.radius, cli.laplaceBoost, cli.dumpPre))
                ++ok;
            else
                ++fail;

            seen.insert(key);
        }
    };

    size_t total_ok=0, total_fail=0;
    scan_once(total_ok, total_fail);

    if(!cli.watch){
        std::cout << "\nDone. Success: " << total_ok << "  Failed: " << total_fail << "\n";
        return (total_fail==0)? 0 : 2;
    }

    std::cout << "\nWatching"
              << (cli.recurse ? " (recursive)" : "")
              << " every " << cli.interval_ms << " ms.  Ctrl+C to stop.\n";

    while(!g_stop){
        scan_once(total_ok, total_fail);
        std::this_thread::sleep_for(std::chrono::milliseconds(std::max(100, cli.interval_ms)));
    }

    std::cout << "\nStopped. Success: " << total_ok << "  Failed: " << total_fail << "\n";
    return (total_fail==0)? 0 : 2;
}
