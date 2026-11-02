// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Experimental diagnostic objective; see docs/encoder-quality.md for its
// intelligibility/PESQ tradeoff. This is not a validated perceptual-quality
// score or part of the public streaming encoder.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <vector>

// Diagnostic auditory-envelope objective. No fitted corpus parameters.
struct PowerBands {
    int size;std::vector<double> cosine,sine,filter;
    explicit PowerBands(int n):size(n),cosine((n/2+1)*n),sine(cosine.size()),filter(24*(n/2+1),0) {
        const double pi=3.141592653589793;
        for(int k=0;k<=n/2;++k)for(int i=0;i<n;++i) {
            double window=.5-.5*std::cos(2*pi*i/(n-1));
            cosine[k*n+i]=window*std::cos(2*pi*k*i/n);sine[k*n+i]=window*std::sin(2*pi*k*i/n);
        }
        double edges[26],lo=2595*std::log10(1+100./700),hi=2595*std::log10(1+3800./700);
        for(int j=0;j<26;++j)edges[j]=700*(std::pow(10.,(lo+(hi-lo)*j/25)/2595)-1);
        for(int j=0;j<24;++j)for(int k=0;k<=n/2;++k) {
            double hz=k*8000./n;
            filter[j*(n/2+1)+k]=std::max(0.,std::min((hz-edges[j])/(edges[j+1]-edges[j]),(edges[j+2]-hz)/(edges[j+2]-edges[j+1])));
        }
    }
    std::array<double,24> analyse(const double* input)const {
        std::vector<double> power(size/2+1);
        for(int k=0;k<=size/2;++k) {
            double re=0,im=0;
            for(int i=0;i<size;++i){re+=input[i]*cosine[k*size+i];im+=input[i]*sine[k*size+i];}
            power[k]=re*re+im*im;
        }
        std::array<double,24> result{};
        for(int j=0;j<24;++j)for(int k=0;k<=size/2;++k)result[j]+=filter[j*(size/2+1)+k]*power[k];
        return result;
    }
};
struct Perceptual {
    PowerBands shortWindow{256},longWindow{512};
    std::array<double,48> source{},previousSource{},previousGenerated{};
    double history[352]={},sourceLevel[2]={},baselineLevel[2]={};bool previousValid=false;
    void begin(const std::vector<short>& input,size_t frame) {
        for(int pass=0;pass<2;++pass) {
            const auto& analyser=pass?longWindow:shortWindow;double window[512]={};
            for(int i=0;i<analyser.size;++i) {
                long pos=static_cast<long>(frame)*160-519+160-analyser.size+i;
                if(pos>=0&&pos<static_cast<long>(input.size()))window[i]=input[pos];
            }
            auto bands=analyser.analyse(window);double total=0;for(auto v:bands)total+=v;
            sourceLevel[pass]=std::max(total,1e-9);
            for(int j=0;j<24;++j)source[pass*24+j]=std::pow(std::max(bands[j]/sourceLevel[pass],1e-5),.3);
        }
    }
    std::array<double,48> features(const float* pcm,int index) {
        std::array<double,48> result{};
        for(int pass=0;pass<2;++pass) {
            const auto& analyser=pass?longWindow:shortWindow;double window[512];
            int past=analyser.size-160;
            for(int i=0;i<past;++i)window[i]=history[352-past+i];
            for(int i=0;i<160;++i)window[past+i]=pcm[i];
            auto bands=analyser.analyse(window);double total=0;for(auto v:bands)total+=v;
            if(index==0)baselineLevel[pass]=std::max(total,1e-9);
            // One level calibration from the original-target trial for ALL candidates.
            // A louder/softer candidate cannot normalise away its own level error.
            for(int j=0;j<24;++j)result[pass*24+j]=std::pow(std::max(bands[j]/baselineLevel[pass],1e-5),.3);
        }
        return result;
    }
    double score(const float* pcm,int index,bool temporal) {
        auto generated=features(pcm,index);double loss=0;
        for(int j=0;j<48;++j) {
            double error=generated[j]-source[j];loss+=error*error;
            if(temporal&&previousValid) {
                double delta=(generated[j]-previousGenerated[j])-(source[j]-previousSource[j]);
                loss+=.5*delta*delta;
            }
        }
        return loss/48;
    }
    void commit(const float* pcm) {
        previousGenerated=features(pcm,-1);previousSource=source;previousValid=true;
        for(int i=0;i<192;++i)history[i]=history[i+160];
        for(int i=0;i<160;++i)history[192+i]=pcm[i];
    }
};
