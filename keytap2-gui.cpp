/*! \file keytap2-gui.cpp
 *  \brief Enter description here.
 *  \author Georgi Gerganov
 */

#include "constants.h"
#include "subbreak.h"
#include "subbreak2.h"

#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"

#include <SDL.h>
#include <GL/gl3w.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <deque>
#include <fstream>
#include <map>
#include <string>
#include <tuple>
#include <vector>
#include <thread>
#include <algorithm>

//#define MY_DEBUG

int g_windowSizeX = 1920;
int g_windowSizeY = 1200;

struct stParameters;
struct stMatch;
struct stKeyPressData;
struct stWaveformView;
struct stKeyPressCollection;

using TKey                  = int;
using TSum                  = int64_t;
using TSum2                 = int64_t;
using TCC                   = double;
using TOffset               = int64_t;
using TMatch                = stMatch;
using TParameters           = stParameters;
using TSimilarityMap        = std::vector<std::vector<TMatch>>;

using TClusterId            = int32_t;
using TSampleInput          = float;
using TSample               = int32_t;
using TWaveform             = std::vector<TSample>;
using TWaveformView         = stWaveformView;
using TKeyPressPosition     = int64_t;
using TKeyPressData         = stKeyPressData;
using TKeyPressCollection   = stKeyPressCollection;

enum Approach {
    ClusterG = 0,
    DBSCAN,
    NonExactSubstitutionCipher0,

    COUNT,
};

std::string getApproachStr(Approach approach) {
    std::string res = "-";
    switch (approach) {
        case ClusterG:
            res = "ClusterG clustering + Substition Cipher attack";
            break;
        case DBSCAN:
            res = "DBSCAN clustering + Substition Cipher attack";
            break;
        case NonExactSubstitutionCipher0:
            res = "Non-Exact Substitution Cipher attack";
            break;
        case COUNT:
            break;
    };
    return res;
}

struct stParameters {
    int keyPressWidth_samples   = 256;
    int sampleRate              = kSampleRate;
    int offsetFromPeak          = keyPressWidth_samples/2;
    int alignWindow             = 256;
    float thresholdClustering   = 0.5f;
    int nMinKeysInCluster       = 3;
    std::string fnameLetterMask = "";

    Approach approach = ClusterG;

    // simulated annealing
    int saMaxIterations = 1000000;
    float temp0 = 1e8;
    float coolingRate = 0.99995;

    Cipher::TParameters cipher;
};

struct stMatch {
    TCC     cc      = 0.0;
    TOffset offset  = 0;
};

struct stWaveformView {
    const TSample * samples     = nullptr;
    int64_t                     n = 0;
};

struct stKeyPressData {
    TWaveformView       waveform;
    TKeyPressPosition   pos         = 0;
    TCC                 ccAvg       = 0.0;
    TClusterId          cid         = -1;
    TKey                bind        = -1;
    TKey                predicted   = '?';
};

struct stKeyPressCollection : public std::vector<TKeyPressData> {
    int nClusters = 0;
};

template <typename T>
float toSeconds(T t0, T t1) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()/1024.0f;
}

TWaveformView getView(const TWaveform & waveform, int64_t idx) { return { waveform.data() + idx, (int64_t) waveform.size() - idx }; }

TWaveformView getView(const TWaveform & waveform, int64_t idx, int64_t len) { return { waveform.data() + idx, len }; }

bool saveKeyPresses(const char * fname, const TKeyPressCollection & keyPresses) {
    std::ofstream fout(fname, std::ios::binary);
    int n = keyPresses.size();
    fout.write((char *)(&n), sizeof(n));
    for (int i = 0; i < n; ++i) {
        fout.write((char *)(&keyPresses[i].pos), sizeof(keyPresses[i].pos));
    }
    fout.close();

    return true;
}

bool loadKeyPresses(const char * fname, const TWaveformView & waveform, TKeyPressCollection & keyPresses) {
    keyPresses.clear();

    std::ifstream fin(fname, std::ios::binary);
    int n = 0;
    fin.read((char *)(&n), sizeof(n));
    keyPresses.resize(n);
    for (int i = 0; i < n; ++i) {
        keyPresses[i].waveform = waveform;
        fin.read((char *)(&keyPresses[i].pos), sizeof(keyPresses[i].pos));
    }
    fin.close();

    return true;
}

bool readFromFile(const std::string & fname, TWaveform & res) {
    std::ifstream fin(fname, std::ios::binary | std::ios::ate);
    if (fin.good() == false) {
        return false;
    }

    {
        std::streamsize size = fin.tellg();
        fin.seekg(0, std::ios::beg);

        static_assert(std::is_same<TSampleInput, float>::value, "TSampleInput not recognised");
        static_assert(
            std::is_same<TSample, float>::value
            || std::is_same<TSample, int16_t>::value
            || std::is_same<TSample, int32_t>::value
                      , "TSampleInput not recognised");

        if (std::is_same<TSample, int16_t>::value) {
            std::vector<TSampleInput> buf(size/sizeof(TSampleInput));
            res.resize(size/sizeof(TSampleInput));
            fin.read((char *)(buf.data()), size);
            double amax = 0.0f;
            for (auto i = 0; i < buf.size(); ++i) if (std::abs(buf[i]) > amax) amax = std::abs(buf[i]);
            for (auto i = 0; i < buf.size(); ++i) res[i] = std::round(32000.0*(buf[i]/amax));
            //double asum = 0.0f;
            //for (auto i = 0; i < buf.size(); ++i) asum += std::abs(buf[i]); asum /= buf.size(); asum *= 10.0;
            //for (auto i = 0; i < buf.size(); ++i) res[i] = std::round(2000.0*(buf[i]/asum));
        } else if (std::is_same<TSample, int32_t>::value) {
            std::vector<TSampleInput> buf(size/sizeof(TSampleInput));
            res.resize(size/sizeof(TSampleInput));
            fin.read((char *)(buf.data()), size);
            double amax = 0.0f;
            for (auto i = 0; i < buf.size(); ++i) if (std::abs(buf[i]) > amax) amax = std::abs(buf[i]);
            for (auto i = 0; i < buf.size(); ++i) res[i] = std::round(32000.0*(buf[i]/amax));
        } else if (std::is_same<TSample, float>::value) {
            res.resize(size/sizeof(TSample));
            fin.read((char *)(res.data()), size);
        } else {
        }
    }

    fin.close();

    return true;
}

bool generateLowResWaveform(const TWaveformView & waveform, TWaveform & waveformLowRes, int nWindow) {
    waveformLowRes.resize(waveform.n);

    int k = nWindow;
    std::deque<int64_t> que(k);

    //auto [samples, n] = waveform;
    auto samples = waveform.samples;
    auto n       = waveform.n;

    TWaveform waveformAbs(n);
    for (int64_t i = 0; i < n; ++i) {
        waveformAbs[i] = std::abs(samples[i]);
    }

    for (int64_t i = 0; i < n; ++i) {
        if (i < k) {
            while((!que.empty()) && waveformAbs[i] >= waveformAbs[que.back()]) {
                que.pop_back();
            }
            que.push_back(i);
        } else {
            while((!que.empty()) && que.front() <= i - k) {
                que.pop_front();
            }

            while((!que.empty()) && waveformAbs[i] >= waveformAbs[que.back()]) {
                que.pop_back();
            }

            que.push_back(i);

            int64_t itest = i - k/2;
            waveformLowRes[itest] = waveformAbs[que.front()];
        }
    }

    return true;
}

bool generateLowResWaveform(const TWaveform & waveform, TWaveform & waveformLowRes, int nWindow) {
    return generateLowResWaveform(getView(waveform, 0), waveformLowRes, nWindow);
}

bool findKeyPresses(const TWaveformView & waveform, TKeyPressCollection & res, TWaveform & waveformThreshold, double thresholdBackground, int historySize) {
    res.clear();
    waveformThreshold.resize(waveform.n);

    int rbBegin = 0;
    double rbAverage = 0.0;
    std::vector<double> rbSamples(8*historySize, 0.0);

    int k = historySize;
    std::deque<int64_t> que(k);

    //auto [samples, n] = waveform;
    auto samples = waveform.samples;
    auto n       = waveform.n;

    TWaveform waveformAbs(n);
    for (int64_t i = 0; i < n; ++i) {
        waveformAbs[i] = std::abs(samples[i]);
    }

    for (int64_t i = 0; i < n; ++i) {
        {
            int64_t ii = i - k/2;
            if (ii >= 0) {
                rbAverage *= rbSamples.size();
                rbAverage -= rbSamples[rbBegin];
                double acur = waveformAbs[i];
                rbSamples[rbBegin] = acur;
                rbAverage += acur;
                rbAverage /= rbSamples.size();
                if (++rbBegin >= rbSamples.size()) {
                    rbBegin = 0;
                }
            }
        }

        if (i < k) {
            while((!que.empty()) && waveformAbs[i] >= waveformAbs[que.back()]) {
                que.pop_back();
            }
            que.push_back(i);
        } else {
            while((!que.empty()) && que.front() <= i - k) {
                que.pop_front();
            }

            while((!que.empty()) && waveformAbs[i] >= waveformAbs[que.back()]) {
                que.pop_back();
            }

            que.push_back(i);

            int64_t itest = i - k/2;
            if (itest >= 2*k && itest < n - 2*k && que.front() == itest) {
                double acur = waveformAbs[itest];
                if (acur > thresholdBackground*rbAverage){
                    TKeyPressData entry;
                    entry.waveform = waveform;
                    entry.pos = itest;
                    entry.ccAvg = 0.0;
                    entry.cid = -1;
                    res.emplace_back(std::move(entry));
                }
            }
            waveformThreshold[itest] = waveformAbs[que.front()];
        }
    }

    return true;
}

bool findKeyPresses(const TWaveform & waveform, TKeyPressCollection & res, TWaveform & waveformThreshold, double thresholdBackground = 10.0, int historySize = 4*1024) {
    return findKeyPresses(getView(waveform, 0), res, waveformThreshold, thresholdBackground, historySize);
}

bool dumpKeyPresses(const std::string & fname, const TKeyPressCollection & data) {
    std::ofstream fout(fname);
    for (auto & k : data) {
        fout << k.pos << " 1" << std::endl;
    }
    fout.close();
    return true;
}

std::tuple<TSum, TSum2> calcSum(const TWaveformView & waveform) {
    TSum sum = 0.0f;
    TSum2 sum2 = 0.0f;

    //auto [samples, n] = waveform;
    auto samples = waveform.samples;
    auto n       = waveform.n;

    for (int64_t is = 0; is < n; ++is) {
        auto a0 = samples[is];
        sum += a0;
        sum2 += a0*a0;
    }

    return std::tuple<TSum, TSum2>(sum, sum2);
}

TCC calcCC(
    const TWaveformView & waveform0,
    const TWaveformView & waveform1,
    TSum sum0, TSum2 sum02) {
    TCC cc = -1.0f;

    TSum sum1 = 0.0f;
    TSum2 sum12 = 0.0f;
    TSum2 sum01 = 0.0f;

    //auto [samples0, n0] = waveform0;
    auto samples0 = waveform0.samples;
    auto n0       = waveform0.n;

    //auto [samples1, n1] = waveform1;
    auto samples1 = waveform1.samples;
    auto n1       = waveform1.n;

#ifdef MY_DEBUG
    if (n0 != n1) {
        printf("BUG 234f8273\n");
    }
#endif
    auto n = std::min(n0, n1);

    for (int64_t is = 0; is < n; ++is) {
        auto a0 = samples0[is];
        auto a1 = samples1[is];

        sum1 += a1;
        sum12 += a1*a1;
        sum01 += a0*a1;
    }

    {
        double nom = sum01*n - sum0*sum1;
        double den2a = sum02*n - sum0*sum0;
        double den2b = sum12*n - sum1*sum1;
        cc = (nom)/(sqrt(den2a*den2b));
    }

    return cc;
}

std::tuple<TCC, TOffset> findBestCC(
    const TWaveformView & waveform0,
    const TWaveformView & waveform1,
    int64_t alignWindow) {
    TCC bestcc = -1.0;
    TOffset besto = -1;

    //auto [samples0, n0] = waveform0;
    //auto samples0 = waveform0.samples;
    auto n0       = waveform0.n;

    //auto [samples1, n1] = waveform1;
    auto samples1 = waveform1.samples;

#ifdef MY_DEBUG
    if (n0 + 2*alignWindow != n1) {
        printf("BUG 924830jm92, n0 = %d, a = %d\n", (int) n0, (int) alignWindow);
    }
#endif

    //auto [sum0, sum02] = calcSum(waveform0);
    auto ret = calcSum(waveform0);
    auto sum0  = std::get<0>(ret);
    auto sum02 = std::get<1>(ret);

    for (int o = 0; o < 2*alignWindow; ++o) {
        auto cc = calcCC(waveform0, { samples1 + o, n0 }, sum0, sum02);
        if (cc > bestcc) {
            besto = o - alignWindow;
            bestcc = cc;
        }
    }

    return std::tuple<TCC, TOffset>(bestcc, besto);
}

bool calculateSimilartyMap(const TParameters & params, TKeyPressCollection & keyPresses, TSimilarityMap & res) {
    res.clear();
    int nPresses = keyPresses.size();

    int w = params.keyPressWidth_samples;
    int alignWindow = params.alignWindow;

    res.resize(nPresses);
    for (auto & x : res) x.resize(nPresses);

    for (int i = 0; i < nPresses; ++i) {
        res[i][i].cc = 1.0f;
        res[i][i].offset = 0;

        //auto & [waveform0, pos0, avgcc, _x] = keyPresses[i];
        auto & waveform0 = keyPresses[i].waveform;
        auto & pos0      = keyPresses[i].pos;
        auto & avgcc     = keyPresses[i].ccAvg;

        //auto [samples0, n0] = waveform0;
        auto samples0 = waveform0.samples;
        //auto n0       = waveform0.n;

        for (int j = 0; j < nPresses; ++j) {
            if (i == j) continue;

            auto waveform1 = keyPresses[j].waveform;
            auto pos1      = keyPresses[j].pos;

            auto samples1 = waveform1.samples;
            //auto [bestcc, bestoffset] = findBestCC({ samples0 + pos0 + params.offsetFromPeak,               2*w },
            //                                       { samples1 + pos1 + params.offsetFromPeak - alignWindow, 2*w + 2*alignWindow }, alignWindow);
            auto ret = findBestCC({ samples0 + pos0 + params.offsetFromPeak,               2*w },
                                  { samples1 + pos1 + params.offsetFromPeak - alignWindow, 2*w + 2*alignWindow }, alignWindow);
            auto bestcc     = std::get<0>(ret);
            auto bestoffset = std::get<1>(ret);

            res[i][j].cc = bestcc;
            res[i][j].offset = bestoffset;

            avgcc += bestcc;
        }
        avgcc /= (nPresses - 1);
    }

    return true;
}

bool clusterDBSCAN(const TSimilarityMap & sim, TCC epsCC, int minPts, TKeyPressCollection & keyPresses) {
    int n = keyPresses.size();
    for (int i = 0; i < n; ++i) {
        auto & cid = keyPresses[i].cid;

        cid = -1;
    }

    int curId = n + 1;

    curId = 0;

    for (int i = 0; i < n; ++i) {
        auto & cid = keyPresses[i].cid;

        if (cid != -1) continue;
        std::vector<int> nbi;
        for (int j = 0; j < n; ++j) {
            auto & cc = sim[i][j].cc;

            if (cc > epsCC) nbi.push_back(j);
        }

        if (nbi.size() < minPts) {
            cid = 0;
            continue;
        }

        cid = ++curId;
        for (int q = 0; q < (int) nbi.size(); ++q) {
            auto & qcid = keyPresses[nbi[q]].cid;

            if (qcid == 0) qcid = curId;
            if (qcid != -1) continue;
            qcid = curId;

            std::vector<int> nbq;
            for (int j = 0; j < n; ++j) {
                auto & cc = sim[nbi[q]][j].cc;

                if (cc > epsCC) nbq.push_back(j);
            }

            if (nbq.size() >= minPts) {
                nbi.insert(nbi.end(), nbq.begin(), nbq.end());
            }
        }
    }

    {
        int nclusters = 0;
        std::map<int, bool> used;
        for (const auto & kp : keyPresses) {
            if (used[kp.cid] == false) {
                used[kp.cid] = true;
                ++nclusters;
            }
        }

        keyPresses.nClusters = nclusters;
    }

    {
        int curId = 0;
        int n = keyPresses.size();
        std::vector<int> newcid(n, 0);
        for (int i = 0; i < n; ++i) {
            if (newcid[i] != 0) continue;
            ++curId;
            for (int j = 0; j < n; ++j) {
                if (keyPresses[i].cid == keyPresses[j].cid) {
                    newcid[j] = curId;
                }
            }
        }

        for (int i = 0; i < n; ++i) {
            keyPresses[i].cid = newcid[i];
        }
    }

    return true;
}

bool clusterG(const TSimilarityMap & sim, TKeyPressCollection & keyPresses, TCC threshold) {
    struct Pair {
        int i = -1;
        int j = -1;
        TCC cc = -1.0;

        bool operator < (const Pair & a) const { return cc > a.cc; }
    };

    int n = keyPresses.size();

    int nclusters = 0;
    for (int i = 0; i < n; ++i) {
        keyPresses[i].cid = i + 1;
        ++nclusters;
    }

    for (int i = 0; i < n; ++i) {
        if (keyPresses[i].bind < 0) continue;
        for (int j = 0; j < n; ++j) {
            if (keyPresses[j].bind == keyPresses[i].bind) {
                if (keyPresses[j].cid != keyPresses[i].cid) {
                    keyPresses[j].cid = keyPresses[i].cid;
                    --nclusters;
                }
            }
        }
    }

    std::vector<Pair> ccpairs;
    for (int i = 0; i < n - 1; ++i) {
        for (int j = i + 1; j < n; ++j) {
            ccpairs.emplace_back(Pair{i, j, sim[i][j].cc});
        }
    }

    std::sort(ccpairs.begin(), ccpairs.end());

    printf("[+] Top 10 pairs\n");
    for (int i = 0; i < 10; ++i) {
        printf("    Pair %d: %d %d %g\n", i, ccpairs[i].i, ccpairs[i].j, ccpairs[i].cc);
    }

    int npairs = ccpairs.size();
    for (int ip = 0; ip < npairs; ++ip) {
        auto & curpair = ccpairs[ip];
        if (frand() > curpair.cc) continue;
        if (curpair.cc < threshold) break;

        auto ci = keyPresses[curpair.i].cid;
        auto cj = keyPresses[curpair.j].cid;

        int bindi = -1;
        int bindj = -1;
        for (int k = 0; k < n; ++k) {
            if (keyPresses[k].cid == ci && keyPresses[k].bind > -1) bindi = keyPresses[k].bind;
            if (keyPresses[k].cid == cj && keyPresses[k].bind > -1) bindj = keyPresses[k].bind;
        }

        if (bindi != bindj && bindi > -1 && bindj > -1) continue;

        if (ci == cj) continue;
        auto cnew = std::min(ci, cj);
        int nsum = 0;
        int nsumi = 0;
        int nsumj = 0;
        double sumcc = 0.0;
        double sumcci = 0.0;
        double sumccj = 0.0;
        for (int k = 0; k < n; ++k) {
            auto & ck = keyPresses[k].cid;
            for (int q = 0; q < n; ++q) {
                if (q == k) continue;
                auto & cq = keyPresses[q].cid;
                if ((ck == ci || ck == cj) && (cq == ci || cq == cj)) {
                    sumcc += sim[k][q].cc;
                    ++nsum;
                }
                if (ck == ci && cq == ci) {
                    sumcci += sim[k][q].cc;
                    ++nsumi;
                }
                if (ck == cj && cq == cj) {
                    sumccj += sim[k][q].cc;
                    ++nsumj;
                }
            }
        }
        sumcc /= nsum;
        if (nsumi > 0) sumcci /= nsumi;
        if (nsumj > 0) sumccj /= nsumj;
        //printf("Merge avg n = %4d, cc = %8.5f, ni = %4d, cci = %8.5f, nj = %4d, ccj = %8.5f\n", nsum, sumcc, nsumi, sumcci, nsumj, sumccj);

        //if (sumcc < 1.000*curpair.cc) continue;
        //if (sumcc > 0.75*sumccj && sumcc > 0.75*sumcci) {
        if (sumcc > 0.4*(sumcci + sumccj)) {
        } else {
            continue;
        }

        for (int k = 0; k < n; ++k) {
            auto & ck = keyPresses[k].cid;
            if (ck == ci || ck == cj) ck = cnew;
        }
        --nclusters;
    }

    keyPresses.nClusters = nclusters;

    {
        int curId = 0;
        int n = keyPresses.size();
        std::vector<int> newcid(n, 0);
        for (int i = 0; i < n; ++i) {
            if (newcid[i] != 0) continue;
            ++curId;
            for (int j = 0; j < n; ++j) {
                if (keyPresses[i].cid == keyPresses[j].cid) {
                    newcid[j] = curId;
                }
            }
        }
        if (curId != nclusters) {
            printf("Something is wrong. curId = %d, nclusters = %d\n", curId, nclusters);
        }

        for (int i = 0; i < n; ++i) {
            keyPresses[i].cid = newcid[i];
        }
    }

    return true;
}

bool adjustKeyPresses(const TParameters & params, TKeyPressCollection & keyPresses, TSimilarityMap & sim) {
    struct Pair {
        int i = -1;
        int j = -1;
        TCC cc = -1.0;

        bool operator < (const Pair & a) const { return cc > a.cc; }
    };

    int n = keyPresses.size();

    std::vector<Pair> ccpairs;
    for (int i = 0; i < n - 1; ++i) {
        for (int j = i + 1; j < n; ++j) {
            ccpairs.emplace_back(Pair{i, j, sim[i][j].cc});
        }
    }

    int nused = 0;
    std::vector<bool> used(n, false);

    std::sort(ccpairs.begin(), ccpairs.end());

    int npairs = ccpairs.size();
    for (int ip = 0; ip < npairs; ++ip) {
        auto & curpair = ccpairs[ip];
        int k0 = curpair.i;
        int k1 = curpair.j;
        if (used[k0] && used[k1]) continue;

        if (used[k1] == false) {
            keyPresses[k1].pos += sim[k0][k1].offset;
        } else {
            keyPresses[k0].pos -= sim[k0][k1].offset;
        }

        if (used[k0] == false) { used[k0] = true; ++nused; }
        if (used[k1] == false) { used[k1] = true; ++nused; }

        if (nused == n) break;
    }

    return true;
}

float plotWaveform(void * data, int i) {
    TWaveformView * waveform = (TWaveformView *)data;
    return waveform->samples[i];
};

float plotWaveformInverse(void * data, int i) {
    TWaveformView * waveform = (TWaveformView *)data;
    return -waveform->samples[i];
};

struct PlaybackData {
    static const int kSamples = 1024;
    int slowDown = 1;
    int64_t idx = 0;
    int64_t offset = 0;
    TWaveformView waveform;
};

SDL_AudioDeviceID g_deviceIdOut = 0;
PlaybackData g_playbackData;

bool renderKeyPresses(TParameters & params, const char * fnameInput, const TWaveform & waveform, TKeyPressCollection & keyPresses) {
    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(g_windowSizeX, 350.0f), ImGuiCond_Once);
    if (ImGui::Begin("Key Presses")) {
        int viewMin = 512;
        int viewMax = waveform.size();

        bool ignoreDelete = false;

        static int nview = waveform.size();
        static int offset = (waveform.size() - nview)/2;
        static float amin = -16000;
        static float amax = 16000;
        static float dragOffset = 0.0f;
        static float scrollSize = 18.0f;

        static auto nviewPrev = nview + 1;

        static TWaveform waveformLowRes = waveform;
        static TWaveform waveformThreshold = waveform;

        auto wview = getView(waveformLowRes, offset, nview);
        auto tview = getView(waveformThreshold, offset, nview);
        auto wsize = ImVec2(ImGui::GetContentRegionAvailWidth(), 250.0);

        auto mpos = ImGui::GetIO().MousePos;
        auto savePos = ImGui::GetCursorScreenPos();
        auto drawList = ImGui::GetWindowDrawList();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, { 0.3f, 0.3f, 0.3f, 0.3f });
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, { 1.0f, 1.0f, 1.0f, 1.0f });
        ImGui::PlotHistogram("##Waveform", plotWaveform, &wview, nview, 0, "Waveform", amin, amax, wsize);
        ImGui::PopStyleColor(2);
        ImGui::SetCursorScreenPos(savePos);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, { 0.1f, 0.1f, 0.1f, 0.0f });
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, { 1.0f, 1.0f, 1.0f, 1.0f });
        ImGui::PlotHistogram("##Waveform", plotWaveformInverse, &wview, nview, 0, "Waveform", amin, amax, wsize);
        ImGui::PopStyleColor(2);
        ImGui::SetCursorScreenPos(savePos);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, { 0.0f, 0.0f, 0.0f, 0.0f });
        ImGui::PushStyleColor(ImGuiCol_PlotLines, { 0.0f, 1.0f, 0.0f, 0.5f });
        ImGui::PlotLines("##WaveformThreshold", plotWaveform, &tview, nview, 0, "", amin, amax, wsize);
        ImGui::PopStyleColor(2);
        ImGui::SetCursorScreenPos(savePos);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, { 0.0f, 0.0f, 0.0f, 0.0f });
        ImGui::PushStyleColor(ImGuiCol_PlotLines, { 0.0f, 1.0f, 0.0f, 0.5f });
        ImGui::PlotLines("##WaveformThreshold", plotWaveformInverse, &tview, nview, 0, "", amin, amax, wsize);
        ImGui::PopStyleColor(2);
        ImGui::SetCursorScreenPos(savePos);
        ImGui::InvisibleButton("##WaveformIB",wsize);
        if (ImGui::IsItemHovered()) {
            auto w = ImGui::GetIO().MouseWheel;
            auto nview_old = nview;
            nview *= (10.0 + w)/10.0;
            nview = std::min(std::max(viewMin, nview), viewMax);
            if (w != 0.0) {
                offset = std::max(0.0f, offset + ((mpos.x - savePos.x)/wsize.x)*(nview_old - nview));
            }

            if (ImGui::IsMouseDown(0) && ImGui::IsMouseDragging(0) == false) {
                dragOffset = offset;
            }

            if (ImGui::IsMouseDragging(0)) {
                offset = dragOffset - ImGui::GetMouseDragDelta(0).x*nview/wsize.x;
            }

            if (ImGui::IsMouseReleased(0) && ImGui::GetIO().KeyCtrl) {
                int i = 0;
                int64_t pos = offset + nview*(mpos.x - savePos.x)/wsize.x;
                for (i = 0; i < keyPresses.size(); ++i) {
                    int64_t p0 = keyPresses[i].pos + params.offsetFromPeak - params.keyPressWidth_samples;
                    int64_t p1 = keyPresses[i].pos + params.offsetFromPeak + params.keyPressWidth_samples;
                    int64_t pmin = std::min(std::min(keyPresses[i].pos, p0), p1);
                    int64_t pmax = std::max(std::max(keyPresses[i].pos, p0), p1);
                    if (pmin > pos) {
                        ignoreDelete = true;
                        TKeyPressData entry;
                        entry.pos = pos;
                        entry.waveform = getView(waveform, 0);
                        keyPresses.insert(keyPresses.begin() + i, entry);
                        break;
                    }
                    if (pos >= pmin && pos <= pmax) break;
                }
                if (i == keyPresses.size() && ignoreDelete == false) {
                    ignoreDelete = true;
                    TKeyPressData entry;
                    entry.pos = pos;
                    entry.waveform = getView(waveform, 0);
                    keyPresses.push_back(entry);
                }
            }
        }
        if (ImGui::BeginPopupContextWindow()) {
            ImGui::SliderInt("View  ", &nview, viewMin, viewMax);
            ImGui::DragInt  ("Offset", &offset, 0.01*nview, 0, waveform.size() - nview);
            ImGui::SliderFloat("Amplitude Min", &amin, -32000, 0);
            ImGui::SliderFloat("Amplitude Max", &amax, 0, 32000);
            ImGui::EndPopup();
        }

        ImGui::InvisibleButton("##WaveformScrollIB", {wsize.x, scrollSize});
        drawList->AddRect({savePos.x, savePos.y + wsize.y}, {savePos.x + wsize.x, savePos.y + wsize.y + scrollSize}, ImGui::ColorConvertFloat4ToU32({1.0f, 1.0f, 1.0f, 1.0f}));
        drawList->AddRectFilled({savePos.x + wsize.x*(1.f*offset)/viewMax, savePos.y + wsize.y}, {savePos.x + wsize.x*(1.f*offset + nview)/viewMax, savePos.y + wsize.y + scrollSize}, ImGui::ColorConvertFloat4ToU32({1.0f, 1.0f, 1.0f, 1.0f}));

        auto savePos2 = ImGui::GetCursorScreenPos();

        static bool scrolling = false;
        if (ImGui::IsItemHovered()) {
            if (ImGui::IsMouseDown(0)) {
                scrolling = true;
            }
        }

        if (scrolling) {
            offset = ((mpos.x - savePos.x)/wsize.x)*viewMax - nview/2;
        }

        if (ImGui::IsMouseDown(0) == false) {
            scrolling = false;
        }

        offset = std::max(0, std::min((int) offset, (int) waveform.size() - nview));
        for (int i = 0; i < (int) keyPresses.size(); ++i) {
            if (keyPresses[i].pos + params.offsetFromPeak + params.keyPressWidth_samples < offset) continue;
            if (keyPresses[i].pos + params.offsetFromPeak - params.keyPressWidth_samples >= offset + nview) break;

            {
                float x0 = ((float)(keyPresses[i].pos - offset))/nview;

                ImVec2 p0 = { savePos.x + x0*wsize.x, savePos.y };
                ImVec2 p1 = { savePos.x + x0*wsize.x, savePos.y + wsize.y };

                drawList->AddLine(p0, p1, ImGui::ColorConvertFloat4ToU32({ 1.0f, 0.0f, 0.0f, 0.8f }), 1.0f);
            }

            {
                float x0 = ((float)(keyPresses[i].pos - offset))/nview;
                float x1 = ((float)(keyPresses[i].pos + params.offsetFromPeak - params.keyPressWidth_samples - offset))/nview;
                float x2 = ((float)(keyPresses[i].pos + params.offsetFromPeak + params.keyPressWidth_samples - offset))/nview;

                ImVec2 p0 = { savePos.x + x0*wsize.x, savePos.y };
                ImVec2 p1 = { savePos.x + x1*wsize.x, savePos.y };
                ImVec2 p2 = { savePos.x + x2*wsize.x, savePos.y + wsize.y };

                float xmin = std::min(std::min(p0.x, p1.x), p2.x);
                float xmax = std::max(std::max(p0.x, p1.x), p2.x);

                bool isHovered = (mpos.x > xmin && mpos.x < xmax && mpos.y > p1.y && mpos.y < p2.y);

                float col = isHovered ? 0.7f : 0.3f;
                drawList->AddRectFilled(p1, p2, ImGui::ColorConvertFloat4ToU32({ 1.0f, 0.0f, 0.0f, col }));

                if (isHovered) {
                    if (ImGui::IsMouseReleased(0) && ImGui::GetIO().KeyCtrl && ignoreDelete == false) {
                        keyPresses.erase(keyPresses.begin() + i);
                        --i;
                    }
                }

                if (nview < 64.0*wsize.x) {
                    ImGui::SetCursorScreenPos({ savePos.x + 0.5f*((x1 + x2)*wsize.x - ImGui::CalcTextSize(std::to_string(i).c_str()).x), savePos.y + wsize.y - ImGui::GetTextLineHeightWithSpacing() });
                    ImGui::Text("%d", i);
                }

            }

            {
                float x0 = ((float)(g_playbackData.offset + g_playbackData.idx - offset))/nview;

                ImVec2 p0 = {savePos.x + x0*wsize.x, savePos.y};
                ImVec2 p1 = {savePos.x + x0*wsize.x, savePos.y + wsize.y};
                drawList->AddLine(p0, p1, ImGui::ColorConvertFloat4ToU32({ 1.0f, 1.0f, 0.0f, 0.3f }));
            }
        }
        for (int i = 0; i < (int) keyPresses.size(); ++i) {
            {
                float x0 = ((float)(keyPresses[i].pos))/viewMax;

                ImVec2 p0 = { savePos.x + x0*wsize.x, savePos.y + wsize.y };
                ImVec2 p1 = { savePos.x + x0*wsize.x, savePos.y + wsize.y + scrollSize };

                drawList->AddLine(p0, p1, ImGui::ColorConvertFloat4ToU32({ 1.0f, 0.0f, 0.0f, 0.3f }));
            }
        }

        ImGui::SetCursorScreenPos(savePos2);

        //auto io = ImGui::GetIO();
        //ImGui::Text("Keys pressed:");   for (int i = 0; i < IM_ARRAYSIZE(io.KeysDown); i++) if (ImGui::IsKeyPressed(i))             { ImGui::SameLine(); ImGui::Text("%d", i); }

        static bool recalculate = true;
        static bool playHalfSpeed = false;
        static int historySize = 6*1024;
        static float thresholdBackground = 10.0;
        ImGui::PushItemWidth(100.0);

        ImGui::Checkbox("x0.5", &playHalfSpeed);
        ImGui::SameLine();
        if (ImGui::Button("Play") || ImGui::IsKeyPressed(44)) { // space
            g_playbackData.slowDown = playHalfSpeed ? 2 : 1;
            g_playbackData.idx = 0;
            g_playbackData.offset = offset;
            g_playbackData.waveform = getView(waveform, offset, std::min((int) (10*params.sampleRate), nview));
            SDL_PauseAudioDevice(g_deviceIdOut, 0);
        }

        if (g_playbackData.idx > g_playbackData.waveform.n) {
            SDL_PauseAudioDevice(g_deviceIdOut, 1);
        }

        ImGui::SameLine();
        ImGui::SliderFloat("Threshold background", &thresholdBackground, 0.1f, 50.0f) && (recalculate = true);
        ImGui::SameLine();
        ImGui::SliderInt("History Size", &historySize, 512, 1024*16) && (recalculate = true);
        ImGui::SameLine();
        if (ImGui::Button("Recalculate") || recalculate) {
            findKeyPresses(waveform, keyPresses, waveformThreshold, thresholdBackground, historySize);
            recalculate = false;
        }

        static std::string filename = std::string(fnameInput) + ".keys";
        ImGui::SameLine();
        ImGui::Text("%s", filename.c_str());

        ImGui::SameLine();
        if (ImGui::Button("Save")) {
            saveKeyPresses(filename.c_str(), keyPresses);
        }

        ImGui::SameLine();
        if (ImGui::Button("Load")) {
            loadKeyPresses(filename.c_str(), getView(waveform, 0), keyPresses);
        }

        ImGui::SameLine();
        ImGui::DragInt("Key width", &params.keyPressWidth_samples, 8, 0, params.sampleRate/10);
        ImGui::SameLine();
        ImGui::DragInt("Peak offset", &params.offsetFromPeak, 8, -params.sampleRate/10, params.sampleRate/10);
        ImGui::SameLine();
        ImGui::DragInt("Align window", &params.alignWindow, 8, 0, params.sampleRate/10);

        ImGui::Text("Letter mask: %s", params.fnameLetterMask.c_str());
        ImGui::SameLine();
        if (ImGui::Button("Filter")) {
            std::ifstream fin(params.fnameLetterMask.c_str());
            int idx = 0;
            while (true) {
                char a;
                fin.read((char *)(&a), sizeof(a));
                if (fin.eof()) break;
                if (a != '.') {
                    keyPresses.erase(keyPresses.begin() + idx);
                    --idx;
                }
                ++idx;
                if (idx >= keyPresses.size()) break;
            }
        }

        ImGui::PopItemWidth();

        if (nview != nviewPrev) {
            generateLowResWaveform(waveform, waveformLowRes, std::max(1.0f, nview/wsize.x));
            nviewPrev = nview;
        }

    }
    ImGui::End();

    return false;
}

bool renderSimilarity(TParameters & params, TKeyPressCollection & keyPresses, TSimilarityMap & similarityMap) {
    ImGui::SetNextWindowPos(ImVec2(0, 350.0f), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(0.5f*g_windowSizeX, g_windowSizeY - 350.0f), ImGuiCond_Once);
    if (ImGui::Begin("Similarity")) {
        int n = similarityMap.size();
        auto wsize = ImGui::GetContentRegionAvail();

        static float bsize = 4.0f;
        static float threshold = 0.3f;
        ImGui::PushItemWidth(100.0);
        if (ImGui::Button("Calculate") || ImGui::IsKeyPressed(6)) { // c
            calculateSimilartyMap(params, keyPresses, similarityMap);
        }
        ImGui::SameLine();
        ImGui::SliderFloat("Size", &bsize, 1.5f, 24.0f);
        ImGui::SameLine();
        if (ImGui::Button("Fit")) {
            bsize = (std::min(wsize.x, wsize.y) - 24.0)/n;
        }
        ImGui::SameLine();
        ImGui::SliderFloat("Threshold", &threshold, 0.0f, 1.0f);
        ImGui::SameLine();
        if (ImGui::Button("Adjust")) {
            adjustKeyPresses(params, keyPresses, similarityMap);
        }
        ImGui::PopItemWidth();

        ImGui::BeginChild("Canvas", { 0, 0 }, 1, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_AlwaysHorizontalScrollbar | ImGuiWindowFlags_AlwaysVerticalScrollbar);
        auto savePos = ImGui::GetCursorScreenPos();
        auto drawList = ImGui::GetWindowDrawList();

        int hoveredId = -1;
        ImGui::InvisibleButton("SimilarityMapIB", { n*bsize, n*bsize });
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < n; ++j) {
                float col = similarityMap[i][j].cc;
                ImVec2 p0 = {savePos.x + j*bsize, savePos.y + i*bsize};
                ImVec2 p1 = {savePos.x + (j + 1)*bsize - 1.0f, savePos.y + (i + 1)*bsize - 1.0f};
                if (similarityMap[i][j].cc > threshold) {
                    drawList->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32({1.0f, 1.0f, 1.0f, col}));
                }
                if (ImGui::IsMouseHoveringRect(p0, {p1.x + 1.0f, p1.y + 1.0f})) {
                    if (i == j) hoveredId = i;
                    if (ImGui::IsMouseDown(0) == false) {
                        ImGui::BeginTooltip();
                        ImGui::Text("[%3d, %3d]\n", keyPresses[i].cid, keyPresses[j].cid);
                        ImGui::Text("[%3d, %3d] = %5.4g\n", i, j, similarityMap[i][j].cc);
                        for (int k = 0; k < n; ++k) {
                            if (similarityMap[i][k].cc > 0.5) ImGui::Text("Offset [%3d, %3d] = %d\n", i, k, (int) similarityMap[i][k].offset);
                        }
                        ImGui::Separator();
                        for (int k = 0; k < n; ++k) {
                            if (similarityMap[k][i].cc > 0.5) ImGui::Text("Offset [%3d, %3d] = %d\n", k, i, (int) similarityMap[k][i].offset);
                        }
                        ImGui::EndTooltip();
                    }
                }
            }
        }

        if (hoveredId >= 0) {
            for (int i = 0; i < n; ++i) {
                if (keyPresses[i].cid == keyPresses[hoveredId].cid) {
                    ImVec2 p0 = {savePos.x + i*bsize, savePos.y + i*bsize};
                    ImVec2 p1 = {savePos.x + (i + 1)*bsize - 1.0f, savePos.y + (i + 1)*bsize - 1.0f};
                    drawList->AddRectFilled(p0, p1, ImGui::ColorConvertFloat4ToU32({1.0f, 0.0f, 0.0f, 1.0f}));
                }
            }
        }

        ImGui::EndChild();
    }
    ImGui::End();
    return false;
}

bool renderClusters(TParameters & params, const Cipher::TFreqMap & freqMap, TKeyPressCollection & keyPresses, TSimilarityMap & similarityMap) {
    ImGui::SetNextWindowPos(ImVec2(0.5f*g_windowSizeX, 350.0f), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(0.5f*g_windowSizeX, g_windowSizeY - 350.0f - 300.0f), ImGuiCond_Once);
    if (ImGui::Begin("Clusters")) {
        if (ImGui::BeginCombo("Approach", getApproachStr(params.approach).c_str())) {
            for (int i = 0; i < Approach::COUNT; ++i) {
                if (ImGui::Selectable(getApproachStr((Approach)(i)).c_str(), params.approach == (Approach)(i))) {
                    params.approach = (Approach)(i);
                }
            }
            ImGui::EndCombo();
        }

        if (params.approach == Approach::DBSCAN) {
            ImGui::SliderFloat("Threshold", &params.thresholdClustering, 0.0f, 1.0f);
            ImGui::SliderInt("N min", &params.nMinKeysInCluster, 0, 8);
        }
        if (params.approach == Approach::ClusterG) {
            ImGui::SliderFloat("Threshold", &params.thresholdClustering, 0.0f, 1.0f);
        }
        if (params.approach == Approach::NonExactSubstitutionCipher0) {
            ImGui::SliderInt("Min Clusters", &params.cipher.minClusters, 2, 27);
            ImGui::SliderInt("Max Clusters", &params.cipher.maxClusters, 3, 34);
            ImGui::SliderFloat("English letter frequency weight", &params.cipher.wEnglishFreq, 0.0f, 20.0f);
            ImGui::SliderFloat("Language model weight", &params.cipher.wLanguageModel, 0.0f, 2.0f);
            ImGui::SliderInt("Iterations", &params.cipher.saMaxIterations, 1000, 50000000);
        }

        ImGui::Text("Clusters: %d\n", keyPresses.nClusters);
        if (ImGui::Button("Guess spaces")) {
            for (auto & key : keyPresses) {
                key.bind = -1;
            }

            float step = 0.05f;
            for (float thold = step; thold < 0.9f; thold += step) {
                clusterG(similarityMap, keyPresses, thold);
                if (keyPresses.nClusters > 10) break;
            }

            int nIters = 1e4;
            int nUnique = 27;
            std::string enc = "";
            std::string decrypted = "";

            int n = keyPresses.size();
            enc.resize(n);
            kN = std::max(nUnique, keyPresses.nClusters);
            std::vector<int> hint(kN + 1, -1);
            for (int i = 0; i < n; ++i) {
                enc[i] = keyPresses[i].cid;
            }
            printText(enc);
            TFreqMap oldFreqMap;
            auto & len  = std::get<0>(oldFreqMap);
            auto & fmap = std::get<1>(oldFreqMap);
            len = freqMap.len;
            pMin = freqMap.pmin;
            fmap = freqMap.prob;

            guessSpaces(oldFreqMap, enc, decrypted, nIters, hint);
            for (int i = 0; i < n; ++i) {
                if (decrypted[i] < 'a' || decrypted[i] > 'z') {
                    keyPresses[i].bind = 26;
                }
            }
        }

        if (params.approach == Approach::NonExactSubstitutionCipher0) {
        } else {
            ImGui::SameLine();
            if (ImGui::Button("Calculate") || ImGui::IsKeyPressed(23)) { // t
                if (params.approach == Approach::ClusterG) {
                    clusterG(similarityMap, keyPresses, params.thresholdClustering);
                } else if (params.approach == Approach::DBSCAN) {
                    clusterDBSCAN(similarityMap, params.thresholdClustering, params.nMinKeysInCluster, keyPresses);
                }

                float cost = 0.0f;
                for (int i = 0; i < (int)(keyPresses.size()); ++i) {
                    for (int j = i+1; j < (int)(keyPresses.size()); ++j) {
                        if (keyPresses[i].cid == keyPresses[j].cid) {
                            cost += 1.0f - similarityMap[i][j].cc;
                        }
                    }
                }
                printf("clustering cost = %g\n", cost);
            }
        }

        ImGui::BeginChildFrame(ImGui::GetID("Hints"), ImGui::GetContentRegionAvail());

        int n = keyPresses.size();
        ImGui::Text("Clusters: %d\n", keyPresses.nClusters);
        ImGui::Text(" id : cid : predicted : bind");
        for (int i = 0; i < n; ++i) {
            ImGui::Text("%3d : %3d : %9c : ", i, keyPresses[i].cid, keyPresses[i].predicted);
            ImGui::SameLine();
            ImGui::PushID(i);

            if (ImGui::Button("Bind")) {
                if (keyPresses[i].predicted >= 'a' && keyPresses[i].predicted <= 'z') {
                    keyPresses[i].bind = keyPresses[i].predicted - 'a';
                }
            }
            ImGui::SameLine();
            ImGui::PushItemWidth(100.0);
            char buf[2] = "-";
            buf[0] = keyPresses[i].bind >= 0 ? (keyPresses[i].bind < 26 ? keyPresses[i].bind + 'a' : '+') : '-';
            if (ImGui::BeginCombo("##bind", buf, 0)) {
                if (ImGui::Selectable("-", keyPresses[i].bind < 0)) {
                    keyPresses[i].bind = -1;
                }
                if (ImGui::Selectable("+", keyPresses[i].bind < 0)) {
                    keyPresses[i].bind = 26;
                }
                for (int j = 'a'; j <= 'z'; ++j) {
                    char buf2[2] = { (char) j, 0 };
                    if (ImGui::Selectable(buf2, j == keyPresses[i].bind + 'a')) {
                        keyPresses[i].bind = j - 'a';
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();
            ImGui::SameLine();
            int w = 32;
            auto cw = ImGui::CalcTextSize("a");
            for (int j = std::max(0, i - w); j <= std::min(n, i + w); ++j) {
                if (j == i && keyPresses[j].bind >= 0) {
                    if (keyPresses[j].bind == 26) {
                        ImGui::TextColored({ 0.0f, 1.0f, 0.0f, 1.0f }, "%c", '.');
                    } else {
                        ImGui::TextColored({ 0.0f, 1.0f, 0.0f, 1.0f }, "%c", keyPresses[j].bind + 'a');
                    }
                } else {
                    ImGui::Text("%c", keyPresses[j].predicted);
                }
                ImGui::SameLine();
                auto pos = ImGui::GetCursorScreenPos();
                ImGui::SetCursorScreenPos({ pos.x - cw.x, pos.y });
            }
            ImGui::Text("%s", "");
            ImGui::PopID();
        }

        ImGui::EndChildFrame();
    }
    ImGui::End();
    return false;
}

bool renderSolution(TParameters & params, const Cipher::TFreqMap & freqMap, TKeyPressCollection & keyPresses, const TSimilarityMap & similarityMap) {
    static std::string decrypted = "";

    ImGui::SetNextWindowPos(ImVec2(0.5f*g_windowSizeX, g_windowSizeY - 300.0f), ImGuiCond_Once);
    ImGui::SetNextWindowSize(ImVec2(0.5f*g_windowSizeX, 300.0f), ImGuiCond_Once);
    if (ImGui::Begin("Solution")) {
        if (params.approach == Approach::NonExactSubstitutionCipher0) {
            if (ImGui::Button("Calculate")) {
                Cipher::TClusters clusters;
                Cipher::TSimilarityMap ccMap;
                Cipher::TClusterToLetterMap clMap;

                int N = keyPresses.size();

                std::vector<int> hint(N, -1);
                for (int i = 0; i < N; ++i) {
                    if (keyPresses[i].bind < 0) continue;
                    hint[i] = keyPresses[i].bind + 1;
                }

                Cipher::generateClusters(params.cipher, N, clusters, hint);
                for (int k0 = 0; k0 < N; ++k0){
                    for (int k1 = 0; k1 < N; ++k1){
                        ccMap[k0][k1] = 0.5f*(similarityMap[k0][k1].cc + similarityMap[k1][k0].cc);
                    }
                }

                //Cipher::doSimulatedAnnealing3(params.cipher, ccMap, clusters, hint);
                //Cipher::doSimulatedAnnealing4(params.cipher, freqMap, clusters, clMap, hint);
                Cipher::doSimulatedAnnealing5(params.cipher, freqMap, ccMap, clusters, clMap, hint);
                //Cipher::subbreak(params.cipher, freqMap, clusters, clMap, hint);

                decrypted = "";
                for (int i = 0; i < N; ++i) {
                    keyPresses[i].cid = clusters[i];
                    if (clMap[clusters[i]] > 0 && clMap[clusters[i]] <= 26) {
                        keyPresses[i].predicted = 'a' + clMap[clusters[i]]-1;
                        printf("%c", 'a'+clMap[clusters[i]]-1);
                        decrypted += 'a'+clMap[clusters[i]]-1;
                    } else {
                        keyPresses[i].predicted = '.';
                        printf(".");
                        decrypted += '.';
                    }
                }
                printf("\n");

                float cost = 0.0f;
                for (int i = 0; i < (int)(keyPresses.size()); ++i) {
                    for (int j = i+1; j < (int)(keyPresses.size()); ++j) {
                        if (keyPresses[i].cid == keyPresses[j].cid) {
                            cost += 1.0f - similarityMap[i][j].cc;
                        }
                    }
                }
                printf("clustering cost = %g\n", cost);
            }
        } else {
            static int nIters = 1e4;
            static int nUnique = 27;
            static std::string enc = "";

            ImGui::SliderInt("Iterations", &nIters, 0, 1e5);
            if (ImGui::Button("Calculate")) {
                int n = keyPresses.size();
                enc.resize(n);
                kN = std::max(nUnique, keyPresses.nClusters);
                std::vector<int> hint(kN + 1, -1);
                for (int i = 0; i < n; ++i) {
                    enc[i] = keyPresses[i].cid;
                    if (keyPresses[i].bind != -1) {
                        hint[keyPresses[i].cid] = keyPresses[i].bind + 1;
                    }
                }
                printText(enc);
                TFreqMap oldFreqMap;
                auto & len  = std::get<0>(oldFreqMap);
                auto & fmap = std::get<1>(oldFreqMap);
                len = freqMap.len;
                pMin = freqMap.pmin;
                fmap = freqMap.prob;

                decrypt(oldFreqMap, enc, decrypted, nIters, hint);
                for (int i = 0; i < n; ++i) keyPresses[i].predicted = decrypted[i];
                printf("Done\n");
            }
        }

        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvailWidth());
        ImGui::Text("%s", decrypted.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::End();

    return false;
}

void cbPlayback(void * userData, uint8_t * stream, int len) {
    PlaybackData * data = (PlaybackData *)(userData);
    auto end = std::min(data->idx + PlaybackData::kSamples/data->slowDown, data->waveform.n);
    auto idx = data->idx;
    auto sidx = 0;
    for (; idx < end; ++idx) {
        int16_t a = data->waveform.samples[idx];
        memcpy(stream + (sidx)*sizeof(a), &a, sizeof(a));
        len -= sizeof(a);
        ++sidx;

        if (data->slowDown == 2) {
            int16_t a2 = data->waveform.samples[idx + 1];
            a = 0.5*(a + a2);
            memcpy(stream + (sidx)*sizeof(a), &a, sizeof(a));
            len -= sizeof(a);
            ++sidx;
        }
    }
    while (len > 0) {
        int16_t a = 0;
        memcpy(stream + (idx - data->idx)*sizeof(a), &a, sizeof(a));
        len -= sizeof(a);
        ++idx;
    }
    data->idx = idx;
}

bool prepareAudioOut(const TParameters & params) {
    if (SDL_Init(SDL_INIT_AUDIO) < 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't initialize SDL: %s\n", SDL_GetError());
        return false;
    }

    int nDevices = SDL_GetNumAudioDevices(SDL_FALSE);
    printf("Found %d playback devices:\n", nDevices);
    for (int i = 0; i < nDevices; i++) {
        printf("    - Playback device #%d: '%s'\n", i, SDL_GetAudioDeviceName(i, SDL_FALSE));
    }

    SDL_AudioSpec playbackSpec;
    SDL_zero(playbackSpec);

    playbackSpec.freq = params.sampleRate;
    playbackSpec.format = AUDIO_S16;
    playbackSpec.channels = 1;
    playbackSpec.samples = PlaybackData::kSamples;
    playbackSpec.callback = cbPlayback;
    playbackSpec.userdata = &g_playbackData;

    SDL_AudioSpec obtainedSpec;
    SDL_zero(obtainedSpec);

    g_deviceIdOut = SDL_OpenAudioDevice(NULL, SDL_FALSE, &playbackSpec, &obtainedSpec, 0);
    if (!g_deviceIdOut) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Couldn't open an audio device for playback: %s!\n", SDL_GetError());
        SDL_Quit();
        return false;
    }

    printf("Opened playback device %d\n", g_deviceIdOut);
    printf("    Frequency:  %d\n", obtainedSpec.freq);
    printf("    Format:     %d\n", obtainedSpec.format);
    printf("    Channels:   %d\n", obtainedSpec.channels);
    printf("    Samples:    %d\n", obtainedSpec.samples);

    SDL_PauseAudioDevice(g_deviceIdOut, 1);

    return true;
}

int main(int argc, char ** argv) {
    srand(time(0));

    printf("Usage: %s recrod.kbd n-gram.txt [letter.mask]\n", argv[0]);
    if (argc < 3) {
        return -1;
    }

    TParameters params;
    TWaveform waveformInput;
    TKeyPressCollection keyPresses;
    TSimilarityMap similarityMap;

    if (argc > 3) {
        printf("Setting letter mask file '%s'\n", argv[3]);
        params.fnameLetterMask = argv[3];
    }

    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER) != 0) {
        printf("Error: %s\n", SDL_GetError());
        return -1;
    }

    if (prepareAudioOut(params) == false) {
        printf("Error: failed to initialize audio playback\n");
        return -2;
    }

    printf("[+] Loading recording from '%s'\n", argv[1]);
    if (readFromFile(argv[1], waveformInput) == false) {
        printf("Specified file '%s' does not exist\n", argv[1]);
        return -1;
    }

    Cipher::TFreqMap freqMap;
    if (Cipher::loadFreqMap(argv[2], freqMap) == false) {
        return -1;
    }

#if __APPLE__
    // GL 3.2 Core + GLSL 150
    const char* glsl_version = "#version 150";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
#else
    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
#endif

    // Create window with graphics context
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_DisplayMode current;
    SDL_GetCurrentDisplayMode(0, &current);
    SDL_Window* window = SDL_CreateWindow("Keytap", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, g_windowSizeX, g_windowSizeY, SDL_WINDOW_OPENGL|SDL_WINDOW_RESIZABLE|SDL_WINDOW_ALLOW_HIGHDPI);
    SDL_GLContext gl_context = SDL_GL_CreateContext(window);
    SDL_GL_SetSwapInterval(1); // Enable vsync

    // Initialize OpenGL loader
    bool err = gl3wInit() != 0;
    if (err) {
        fprintf(stderr, "Failed to initialize OpenGL loader!\n");
        return 1;
    }

    // Setup Dear ImGui binding
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    //io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;  // Enable Keyboard Controls

    ImGui::GetStyle().AntiAliasedFill = false;
    ImGui::GetStyle().AntiAliasedLines = false;

    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init(glsl_version);

    printf("[+] Loaded recording: of %d samples (sample size = %d bytes)\n", (int) waveformInput.size(), (int) sizeof(TSample));
    printf("    Size in memory:          %g MB\n", (float)(sizeof(TSample)*waveformInput.size())/1024/1024);
    printf("    Sample size:             %d\n", (int) sizeof(TSample));
    printf("    Total number of samples: %d\n", (int) waveformInput.size());
    printf("    Recording length:        %g seconds\n", (float)(waveformInput.size())/params.sampleRate);

    bool finishApp = false;
    while (finishApp == false) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            ImGui_ImplSDL2_ProcessEvent(&event);
            switch (event.type) {
                case SDL_QUIT:
                    finishApp = true;
                    break;
                case SDL_KEYDOWN:
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        finishApp = true;
                    }
                    break;
                case SDL_WINDOWEVENT:
                    if (event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window)) finishApp = true;
                    break;
            };
        }

        SDL_GetWindowSize(window, &g_windowSizeX, &g_windowSizeY);

        auto tStart = std::chrono::high_resolution_clock::now();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame(window);
        ImGui::NewFrame();

        for (int i = 0; i < IM_ARRAYSIZE(io.KeysDown); i++) if (ImGui::IsKeyPressed(i)) {
            printf("Key pressed: %d\n", i);
        }

        renderKeyPresses(params, argv[1], waveformInput, keyPresses);
        renderSimilarity(params, keyPresses, similarityMap);
        renderClusters(params, freqMap, keyPresses, similarityMap);
        renderSolution(params, freqMap, keyPresses, similarityMap);

        ImGui::Text("Application average %.3f ms/frame (%.1f FPS)", 1000.0f / ImGui::GetIO().Framerate, ImGui::GetIO().Framerate);

        ImGui::Render();
        SDL_GL_MakeCurrent(window, gl_context);
        glViewport(0, 0, (int) io.DisplaySize.x, (int) io.DisplaySize.y);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(window);

        // stupid hack to limit frame-rate to ~60 fps on Mojave
        auto tEnd = std::chrono::high_resolution_clock::now();
        auto tus = std::chrono::duration_cast<std::chrono::microseconds>(tEnd - tStart).count();
        while (tus < 1e6/60.0) {
            std::this_thread::sleep_for(std::chrono::microseconds(std::max(100, (int) (0.5*(1e6/60.0 - tus)))));
            tEnd = std::chrono::high_resolution_clock::now();
            tus = std::chrono::duration_cast<std::chrono::microseconds>(tEnd - tStart).count();
        }
    }

    return 0;
}
