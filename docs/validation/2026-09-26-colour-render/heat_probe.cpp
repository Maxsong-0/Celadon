#include "dsp/ColorModule.h"
#include "GlazeBehaviourReport.cpp"
#include <cstdio>
#include <complex>
using namespace celadon;
using namespace celadon::test;
static Signal colorRun(const Signal& in, float heat) {
 ColorModule m; ColorModule::Params p; p.heat=heat; m.setParams(p); m.prepare({48000,512,2});
 auto out=in; juce::AudioBuffer<float> b(2,512);
 for(int s=0;s<(int)in[0].size();s+=512) {int n=std::min(512,(int)in[0].size()-s); b.setSize(2,n,false,false,true); for(int c=0;c<2;++c)b.copyFrom(c,0,in[c].data()+s,n); m.process(b); for(int c=0;c<2;++c)std::copy_n(b.getReadPointer(c),n,out[c].data()+s);}
 return out;
}
static double amp(const Signal& x,double f) {
 std::complex<double> sum=0.; for(int n=48000;n<96000;++n)sum+=(double)x[0][n]*std::polar(1.,-juce::MathConstants<double>::twoPi*f*n/48000.);return std::abs(sum)*2/48000.;
}
int main(int argc,char**) {
 if(argc>1) {auto music=makeMusicLike(); for(float level:{-18.f,-12.f,-6.f,0.f,6.f}) {auto in=pushedToTruePeak(music,level); auto ref=measureSignal(colorRun(in,0)); for(float h:{0.f,5.f,10.f,15.f,20.f,30.f,40.f,60.f,100.f}){auto m=measureSignal(colorRun(in,h));std::printf("TABLE %.0f %.0f %.5f %.5f\n",level,h,m.integratedLufs-ref.integratedLufs,m.truePeakDb-ref.truePeakDb);}} return 0; }
 for(float level:{-18.f,-6.f,-.5f,6.f}) for(float h:{0.f,10.f,25.f,50.f,100.f}) {
 auto in=makeSine(2,48000,1000,juce::Decibels::decibelsToGain(level),2.);auto out=colorRun(in,h);double fundamental=amp(out,1000),hs=0.;for(int k=2;k<=12;k++)hs+=std::pow(amp(out,1000*k),2);std::printf("THD level=%.1f heat=%.0f thd=%.4f%% fundamental_delta=%.4fdB\n",level,h,100*std::sqrt(hs)/fundamental,20*std::log10(fundamental/amp(in,1000)));
 }
 auto in=makeSine(2,48000,60,.75f,2.);auto hi=makeSine(2,48000,7000,.18f,2.);for(int c=0;c<2;c++)for(int n=0;n<96000;n++)in[c][n]+=hi[c][n];
 for(float h:{0.f,10.f,25.f,50.f,100.f}){auto out=colorRun(in,h);double im=std::sqrt(std::pow(amp(out,6880),2)+std::pow(amp(out,7120),2));std::printf("IMD heat=%.0f sideband=%.4fdBc hf_delta=%.4fdB\n",h,20*std::log10(im/amp(out,7000)),20*std::log10(amp(out,7000)/.18));}
}
