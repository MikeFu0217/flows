#include "engine/sample.hpp"

#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace flows;
using Bytes = std::vector<unsigned char>;
void check(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
void near(float a, double b) { check(std::abs(double(a)-b) < 1e-6, "incorrect decoded PCM"); }
template<class F> void rejects(F f, const char* context) { bool threw=false; try { f(); } catch(const std::runtime_error&) { threw=true; } check(threw,context); }
void number(Bytes& b, std::uint64_t n, unsigned size, bool little=true) {
    for(unsigned i=0;i<size;++i) b.push_back(static_cast<unsigned char>(n >> (8*(little ? i : size-1-i))));
}
void tag(Bytes& b,const char* t) { b.insert(b.end(),t,t+4); }
void set32(Bytes& b,unsigned at,std::uint32_t n,bool little=true) { for(unsigned i=0;i<4;++i) b[at+i]=static_cast<unsigned char>(n >> (8*(little ? i : 3-i))); }
Bytes wave(unsigned bits, unsigned channels, bool floating, const Bytes& pcm) {
    Bytes b; tag(b,"RIFF"); number(b,36+pcm.size()+(pcm.size()&1),4); tag(b,"WAVE"); tag(b,"fmt "); number(b,16,4);
    number(b,floating ? 3 : 1,2); number(b,channels,2); number(b,48000,4); number(b,48000*channels*(bits/8),4);
    number(b,channels*(bits/8),2); number(b,bits,2); tag(b,"data"); number(b,pcm.size(),4); b.insert(b.end(),pcm.begin(),pcm.end());
    if(pcm.size()&1) b.push_back(0); return b;
}
Bytes aiff(unsigned bits, unsigned channels, const Bytes& pcm, const char* compression=nullptr) {
    Bytes comm; number(comm,channels,2,false); number(comm,pcm.size()/(channels*(bits/8)),4,false); number(comm,bits,2,false);
    number(comm,0x400e,2,false); number(comm,0xbb80000000000000ULL,8,false); // 48000 in IEEE extended precision.
    if(compression) { tag(comm,compression); number(comm,0,2,false); }
    Bytes b; tag(b,"FORM"); number(b,0,4,false); tag(b,compression ? "AIFC" : "AIFF"); tag(b,"COMM"); number(b,comm.size(),4,false);
    b.insert(b.end(),comm.begin(),comm.end()); tag(b,"SSND"); number(b,8+pcm.size(),4,false); number(b,0,8,false); b.insert(b.end(),pcm.begin(),pcm.end());
    if(pcm.size()&1) b.push_back(0); set32(b,4,b.size()-8,false); return b;
}
struct Fixture {
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("flows-sample-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Fixture() { std::filesystem::create_directories(root/"Samples"); }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(root,error); }
    std::filesystem::path write(const std::string& name,const Bytes& bytes) {
        const auto path=root/name; std::ofstream out(path,std::ios::binary); out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size()); out.close(); return path;
    }
};
void formats(Fixture& f) {
    for(auto bits : {8U,16U,24U,32U}) {
        Bytes pcm; const auto sign=std::uint64_t(1)<<(bits-1);
        for(auto sample : {sign, std::uint64_t(0), sign/2, sign-1}) number(pcm,sample,bits/8);
        const auto sample=load_sample_file(f.write("pcm.wav",wave(bits,1,false,pcm)));
        check(sample->sample_rate==48000 && sample->left.size()==4 && sample->right.empty(),"WAVE metadata");
        if(bits==8) { near(sample->left[0],0); near(sample->left[1],-1); near(sample->left[2],-.5); }
        else { near(sample->left[0],-1); near(sample->left[1],0); near(sample->left[2],.5); }
        pcm.clear(); for(auto n : {sign, std::uint64_t(0), sign/2, sign-1}) number(pcm,n,bits/8,false);
        const auto ai=load_sample_file(f.write("pcm.aiff",aiff(bits,1,pcm)));
        near(ai->left[0],-1); near(ai->left[1],0); near(ai->left[2],.5); check(ai->sample_rate==48000,"AIFF extended sample rate");
    }
    for(auto bits : {32U,64U}) {
        Bytes pcm, big;
        for(double value : {.25,-.5,1.25,-1.75}) {
            const auto raw=bits==32 ? std::uint64_t(std::bit_cast<std::uint32_t>(static_cast<float>(value))) : std::bit_cast<std::uint64_t>(value);
            number(pcm,raw,bits/8); number(big,raw,bits/8,false);
        }
        const auto wav=load_sample_file(f.write("float.wav",wave(bits,2,true,pcm)));
        near(wav->left[0],.25); near(wav->right[0],-.5); near(wav->left[1],1.25); near(wav->right[1],-1.75);
        const auto ai=load_sample_file(f.write("float.aifc",aiff(bits,2,big,bits==32 ? "fl32" : "fl64")));
        check(ai->left==wav->left && ai->right==wav->right,"AIFC float decoding");
    }
    Bytes little; number(little,0x8000,2); number(little,0x4000,2);
    const auto sowt=load_sample_file(f.write("little.aifc",aiff(16,1,little,"sowt"))); near(sowt->left[0],-1); near(sowt->left[1],.5);
    auto ext=wave(16,1,false,little); ext[20]=0xfe; ext[21]=0xff; set32(ext,16,40);
    Bytes extra; number(extra,22,2); number(extra,16,2); number(extra,4,4); number(extra,1,4); number(extra,0,2); number(extra,0x10,2);
    for(auto byte : {0x80,0,0,0xaa,0,0x38,0x9b,0x71}) extra.push_back(byte);
    ext.insert(ext.begin()+36,extra.begin(),extra.end()); set32(ext,4,ext.size()-8);
    const auto extended=load_sample_file(f.write("extended.wav",ext)); near(extended->left[0],-1); near(extended->left[1],.5);
}
void malformed(Fixture& f) {
    Bytes pcm; number(pcm,0x4000,2); auto bytes=wave(16,1,false,pcm);
    for(std::size_t size=0;size<bytes.size();++size) {
        Bytes cut(bytes.begin(),bytes.begin()+size);
        rejects([&]{ load_sample_file(f.write("cut.wav",cut)); },"truncated WAVE accepted");
    }
    auto bad=bytes; set32(bad,40,0xffffffff); rejects([&]{load_sample_file(f.write("bad.wav",bad));},"oversized chunk accepted");
    bad=bytes; bad[32]=9; rejects([&]{load_sample_file(f.write("bad.wav",bad));},"invalid stride accepted");
    bad=bytes; bad[22]=3; rejects([&]{load_sample_file(f.write("bad.wav",bad));},"multichannel sample accepted");
    bad=bytes; bad[20]=2; rejects([&]{load_sample_file(f.write("bad.wav",bad));},"compressed WAV accepted");
    bad=bytes; set32(bad,24,0); rejects([&]{load_sample_file(f.write("bad.wav",bad));},"zero sample rate accepted");
    pcm.clear(); number(pcm,std::bit_cast<std::uint32_t>(std::numeric_limits<float>::quiet_NaN()),4);
    rejects([&]{load_sample_file(f.write("nan.wav",wave(32,1,true,pcm)));},"NaN sample accepted");
    pcm.clear(); number(pcm,std::bit_cast<std::uint64_t>(1e300),8);
    rejects([&]{load_sample_file(f.write("overflow.wav",wave(64,1,true,pcm)));},"overflowing sample accepted");
    bad=aiff(16,1,Bytes{0,1}); bad[28]=0x7f; bad[29]=0xff;
    rejects([&]{load_sample_file(f.write("bad.aiff",bad));},"invalid extended rate accepted");
    bad=aiff(16,1,Bytes{0,1}); set32(bad,22,1000,false);
    rejects([&]{load_sample_file(f.write("bad.aiff",bad));},"truncated AIFF frames accepted");
    auto path=f.write("good.wav",bytes); SampleLimits limits; limits.max_file_bytes=12;
    rejects([&]{load_sample_file(path,limits);},"file budget ignored");
    limits={}; limits.max_decoded_bytes=1; rejects([&]{load_sample_file(path,limits);},"decoded budget ignored");
    limits={}; limits.max_seconds=1e-10; rejects([&]{load_sample_file(path,limits);},"duration budget ignored");
}
void resources(Fixture& f) {
    f.write("Samples/tone.wav",wave(8,1,false,Bytes{0,128,192,255}));
    Program p; p.source_path=(f.root/"nested"/"instrument.flows").string(); p.samples={{"Samples/tone.wav",{}},{"Samples/tone.wav",{}}};
    auto bank=load_sample_bank(p,f.root); check(bank.size()==2 && bank[0]==bank[1],"resources must resolve workspace root and deduplicate");
    p.source_path=(f.root/"instrument.flows").string(); check(load_sample_bank(p).size()==2,"CLI source-relative fallback");
    auto key=sample_bank_key(bank); check(key==sample_bank_key(load_sample_bank(p)),"PCM identity not deterministic");
    f.write("Samples/tone.wav",wave(8,1,false,Bytes{0,128,190,255})); check(key!=sample_bank_key(load_sample_bank(p)),"PCM changes did not change bank identity");
    for(const auto* path : {"../escape.wav","/tmp/file.wav","./Samples/tone.wav","Samples/../Samples/tone.wav","Samples//tone.wav","https://host/sample.wav","C:\\sample.wav","Samples/missing.wav"}) {
        p.samples={{path,{}}}; rejects([&]{load_sample_bank(p,f.root);},"invalid resource path accepted");
    }
    p.samples={{std::string("Samples/tone.wav\0hidden",23),{}}}; rejects([&]{load_sample_bank(p,f.root);},"NUL path accepted");
    std::filesystem::create_symlink(f.root.parent_path(),f.root/"Samples"/"outside");
    p.samples={{"Samples/outside",{}}}; rejects([&]{load_sample_bank(p,f.root);},"symlink root escape accepted");
    p.samples={{"Samples/tone.wav",{}}}; SampleLimits limits; limits.max_bank_bytes=1;
    rejects([&]{load_sample_bank(p,f.root,limits);},"bank memory budget ignored");
    p.samples.resize(65,p.samples[0]); rejects([&]{load_sample_bank(p,f.root);},"resource count limit ignored");
}
}
int main() {
    try { Fixture f; formats(f); malformed(f); resources(f); std::cout<<"Sample file tests passed\n"; return 0; }
    catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
