#include "device_init.h"
#include "shim_init_diagnostics.h"
#include <string>
#include <cassert>
#include <cstdio>
using namespace mac_hsa;
struct RPC final: ShimInitializationRPC {
 bool ready=false, busy=false, badWindow=false, badUsage=false, failReserve=false, failRelease=false;
 // Compute sessions (driver build 192, QueryInfo tag 12): session reports
 // the tag; kfd makes this client a KFD process whose host window is its
 // own (selector 54 then answers for that window once tag 12 was asked).
 bool session=false, kfd=false, sessionAsked=false, failKFDReserve=false;
 // The driver has tag 12 but gives this client no session: an error, or
 // an answer with no mode.
 bool sessionRefused=false, sessionNoMode=false;
 uint64_t kfdWindowBytes=1ull<<37, kfdBase=0, kfdCandidate=0x40000000000ull;
 unsigned kfdConfigs=0, kfdReserves=0;
 unsigned claim=0,config=0,init=0,reserve=0,releases=0,queries=0;
 // On-demand firmware servicer: must be running exactly while selector 9 is
 // in flight, and stopped on every exit path.
 bool startFails=false, serving=false;
 unsigned fwStarts=0,fwStops=0,servedInit=0;
 uint64_t failSelector=~0ull;
 // The driver's GART aperture: the runtime must reserve whatever size it
 // reports, never an assumed one.
 uint64_t windowBytes=512ull<<20, base=0x200000000ull, vendor=0x1002, device=0x1234;
 // RuntimeBuild handshake answer: any other magic or ABI is a foreign driver.
 uint64_t magic=0x414d444750554142ull, abi=1;
 hsa_status_t failureStatus=HSA_STATUS_ERROR;
 hsa_status_t scalar(uint32_t selector,std::span<const uint64_t> in,std::span<uint64_t> out) override {
  if(selector==9 && serving)servedInit++;
  if(selector!=9)assert(!serving);
  if(selector==failSelector)return failureStatus;
  switch(selector) {
  case 43:out[0]=magic;out[1]=abi;out[2]=ready?(session?192:190):0;break;
  case 1:if(busy)return HSA_STATUS_ERROR_OUT_OF_RESOURCES;claim++;out[3]=vendor;out[4]=device;out[6]=0x07;break;
  case 54:
   if(kfd&&sessionAsked){
    assert(ready);
    if(in[0]){assert(in[0]==kfdCandidate&&!kfdBase);kfdBase=in[0];kfdConfigs++;}
    out[0]=kfdBase;out[1]=kfdWindowBytes;out[2]=1;break;
   }
   if(in[0]){assert(claim && !ready && in[0]==base);config++;} else queries++;
   out[0]=(config||ready)?base:0;out[1]=windowBytes;out[2]=ready&&!badWindow;break;
  case 9:assert(claim && config);init++;ready=true;break;
  case 21:
   if(in[0]==4)out[0]=ready?2:0;
   else if(in[0]==2){out[0]=256ull<<20;out[1]=32ull<<30;}
   else if(in[0]==9){out[0]=32ull<<30;out[1]=31ull<<30;out[2]=1ull<<30;
    out[3]=badUsage?33ull<<30:0;out[4]=256ull<<20;out[5]=0;}
   else if(in[0]==12&&session&&sessionRefused){assert(ready);return HSA_STATUS_ERROR_OUT_OF_RESOURCES;}
   else if(in[0]==12&&session&&sessionNoMode){assert(ready);out[0]=1;out[1]=0;}
   else if(in[0]==12&&session){assert(ready);sessionAsked=true;
    out[0]=1;out[1]=kfd?2:1;out[2]=kfd?127:1;out[3]=kfd?4242:0;
    out[4]=kfd?kfdBase:base;out[5]=kfd?kfdWindowBytes:windowBytes;}
   else assert(false);
   break;
  default:assert(false);
  }
  return HSA_STATUS_SUCCESS;
 }
 hsa_status_t reserveHostWindow(uint64_t n,uint64_t &reserved)override{
  if(kfd&&sessionAsked){assert(n==kfdWindowBytes);kfdReserves++;reserved=kfdCandidate;
   return failKFDReserve?HSA_STATUS_ERROR_OUT_OF_RESOURCES:HSA_STATUS_SUCCESS;}
  assert(claim&&queries&&n==windowBytes);reserve++;reserved=base;
  return failReserve?HSA_STATUS_ERROR_OUT_OF_RESOURCES:HSA_STATUS_SUCCESS;
 }
 hsa_status_t releaseHostWindow(uint64_t reserved,uint64_t n)override{
  if(kfd&&sessionAsked){assert(reserved==kfdCandidate&&n==kfdWindowBytes);releases++;return HSA_STATUS_SUCCESS;}
  assert(reserved==base&&n==windowBytes);releases++;
  return failRelease?HSA_STATUS_ERROR:HSA_STATUS_SUCCESS;
 }
 bool startFirmwareService()override{
  assert(!serving&&claim&&config&&!init);fwStarts++;
  if(startFails)return false;
  serving=true;return true;
 }
 void stopFirmwareService()override{assert(serving&&!startFails);serving=false;fwStops++;}
 // Converted-protocol firmware upload is never used on the shim path.
 hsa_status_t prepareFirmware(const std::vector<FirmwareFile>&)override{assert(false);return HSA_STATUS_ERROR;}
 hsa_status_t uploadFirmware(const FirmwareFile&)override{assert(false);return HSA_STATUS_ERROR;}
 void waitAfterReset()override{assert(false);}
};
int main(){
 constexpr auto bundle="com.geramyloveless.MacAMDGPUHost.MacAMDGPU";
 static_assert(driverProtocol(bundle,"MacLinuxGPU")==DriverProtocol::LinuxShim);
 static_assert(driverProtocol(bundle,"MacAMDGPU")==DriverProtocol::Converted);
 static_assert(driverProtocol(bundle,"MacLinuxGPUUserClient")==DriverProtocol::Unknown);
 static_assert(driverProtocol(bundle,"")==DriverProtocol::Unknown);
 // The shim is found by class under any identifier (an app-embedded dext has
 // its own); the RuntimeBuild handshake below is what rejects an impostor.
 static_assert(driverProtocol("com.geramyloveless.LemonSeedStudio.AMDGpuDriver","MacLinuxGPU")==DriverProtocol::LinuxShim);
 static_assert(driverProtocol("other.driver","MacLinuxGPU")==DriverProtocol::LinuxShim);
 static_assert(driverProtocol("","MacLinuxGPU")==DriverProtocol::Unknown);
 // The converted protocol has no handshake before its firmware upload, so it
 // stays bound to the identifier it shipped under.
 static_assert(driverProtocol("com.geramyloveless.LemonSeedStudio.AMDGpuDriver","MacAMDGPU")==DriverProtocol::Unknown);
 static_assert(driverProtocol("other.driver","MacAMDGPU")==DriverProtocol::Unknown);
 ShimInitializationResult out;bool claimed;
 // A service that merely shares the class name fails the handshake before
 // the session is claimed or any host VA is reserved.
 for(int foreign=0;foreign<2;++foreign){
  RPC r;if(foreign)r.abi=2;else r.magic=0x1234;
  claimed=false;
  assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
  assert(!claimed&&!r.claim&&!r.reserve&&!r.config&&!r.init);}
 {RPC r;assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_SUCCESS);
  assert(claimed&&r.claim==1&&r.init==1&&r.reserve==1&&r.releases==1&&out.capacity==(31ull<<30));}
 {RPC r;r.ready=true;assert(initializeShimDevice(r,out,claimed,false)==HSA_STATUS_SUCCESS);
  assert(claimed&&!r.init&&!r.reserve&&!r.config);}
 // Window size and placement follow the driver's report: 256 MiB and 1 GiB
 // apertures (size-aligned bases) initialize; the result carries the size.
 for(uint64_t bytes:{256ull<<20,1ull<<30,64ull<<20}){
  RPC r;r.windowBytes=bytes;r.base=0x400000000ull;
  assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_SUCCESS);
  assert(r.reserve==1&&r.releases==1&&out.hostBytes==bytes&&out.hostBase==r.base);
  RPC ready;ready.ready=true;ready.windowBytes=bytes;ready.base=0x400000000ull;
  assert(initializeShimDevice(ready,out,claimed,false)==HSA_STATUS_SUCCESS&&out.hostBytes==bytes);}
 // A size the window cannot use (not a power of two, below a host page) is refused
 // before any host VA is reserved.
 for(uint64_t bytes:{0ull,(512ull<<20)+4096,4096ull}){
  RPC r;r.windowBytes=bytes;
  assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
  assert(!r.reserve&&!r.config&&!r.init);}
 // Any AMD device ID is accepted; another vendor is not.
 {RPC r;r.device=0x7fff;assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_SUCCESS);}
 {RPC r;r.vendor=0x10de;assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
  assert(claimed&&!r.reserve);}
 {RPC r;r.busy=true;assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);
  assert(!claimed&&!r.init&&!r.reserve&&!r.config&&!out.capacity);}
 {RPC r;assert(initializeShimDevice(r,out,claimed,false)!=HSA_STATUS_SUCCESS);assert(!r.reserve&&!r.init);}
 {RPC r;r.failSelector=9;
  assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);assert(r.releases==1&&!out.capacity);}
 // The aperture-size query is the first selector-54 call: failing it
 // reserves nothing.
 {RPC r;r.failSelector=54;
  assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);assert(!r.reserve&&!r.releases&&!out.capacity);}
 {RPC r;r.failReserve=true;assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);assert(!r.init&&!r.releases);}
 {RPC r;r.failRelease=true;assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);assert(!out.capacity);}
 {RPC r;r.ready=true;r.badWindow=true;assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);assert(!r.init&&!r.config);}
 {RPC r;r.ready=true;r.badUsage=true;assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);assert(!out.capacity);}
 for(uint32_t selector:{43u,1u,21u,54u,9u}){RPC r;r.failSelector=selector;
  ShimInitializationFailure failure;
  assert(initializeShimDevice(r,out,claimed,true,&failure)==HSA_STATUS_ERROR);
  assert(failure.operation==ShimInitializationOperation::Scalar&&failure.selector==selector);
  assert(failure.status==HSA_STATUS_ERROR);
  assert(failure.hasTag==(selector==21));if(selector==21)assert(failure.tag==4);}
 {RPC r;r.failSelector=9;r.failureStatus=HSA_STATUS_ERROR_OUT_OF_RESOURCES;r.failRelease=true;
  ShimInitializationFailure failure;
  assert(initializeShimDevice(r,out,claimed,true,&failure)==HSA_STATUS_ERROR);
  assert(failure.operation==ShimInitializationOperation::Scalar&&failure.selector==9);
  assert(failure.status==HSA_STATUS_ERROR_OUT_OF_RESOURCES&&r.releases==1);}
 {RPC r;r.failReserve=true;ShimInitializationFailure failure;
  assert(initializeShimDevice(r,out,claimed,true,&failure)==HSA_STATUS_ERROR_OUT_OF_RESOURCES);
  assert(failure.operation==ShimInitializationOperation::ReserveHostWindow&&failure.selector==UINT32_MAX);}
 {RPC r;r.failRelease=true;ShimInitializationFailure failure;
  assert(initializeShimDevice(r,out,claimed,true,&failure)==HSA_STATUS_ERROR);
  assert(failure.operation==ShimInitializationOperation::ReleaseHostWindow&&failure.selector==UINT32_MAX);}
 {RPC r;r.ready=true;r.badUsage=true;ShimInitializationFailure failure;
  assert(initializeShimDevice(r,out,claimed,true,&failure)==HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
  assert(failure.operation==ShimInitializationOperation::Validation&&failure.selector==21&&failure.tag==9);}
 {RPC r;ShimInitializationFailure failure{ShimInitializationOperation::Scalar,9};
  assert(initializeShimDevice(r,out,claimed,true,&failure)==HSA_STATUS_SUCCESS);
  assert(failure.operation==ShimInitializationOperation::None&&failure.status==HSA_STATUS_SUCCESS);}
 // Firmware servicer: started before selector 9, stopped after it on
 // success and failure, never left running, never used without InitDevice.
 {RPC r;assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_SUCCESS);
  assert(r.fwStarts==1&&r.fwStops==1&&r.servedInit==1&&!r.serving);}
 for(hsa_status_t failure:{HSA_STATUS_ERROR,HSA_STATUS_ERROR_OUT_OF_RESOURCES}){
  RPC r;r.failSelector=9;r.failureStatus=failure;
  assert(initializeShimDevice(r,out,claimed)==failure);
  assert(r.fwStarts==1&&r.fwStops==1&&r.servedInit==1&&!r.serving&&r.releases==1);}
 {RPC r;r.failSelector=9;r.failRelease=true;
  assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);
  assert(r.fwStarts==1&&r.fwStops==1&&!r.serving);}
 // A servicer that cannot start does not block initialization (the driver
 // keeps its embedded fallback) and is not stopped.
 {RPC r;r.startFails=true;assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_SUCCESS);
  assert(r.fwStarts==1&&!r.fwStops&&r.init==1&&!r.servedInit);}
 {RPC r;r.startFails=true;r.failSelector=9;
  assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);assert(r.fwStarts==1&&!r.fwStops);}
 // No InitDevice, no servicer: ready reuse and every pre-probe failure.
 {RPC r;r.ready=true;assert(initializeShimDevice(r,out,claimed,false)==HSA_STATUS_SUCCESS);
  assert(!r.fwStarts&&!r.fwStops);}
 for(uint32_t selector:{43u,1u,21u,54u}){RPC r;r.failSelector=selector;
  assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);assert(!r.fwStarts&&!r.fwStops);}
 {RPC r;r.failReserve=true;assert(initializeShimDevice(r,out,claimed)!=HSA_STATUS_SUCCESS);
  assert(!r.fwStarts);}
 // Compute sessions. A legacy client keeps the GART window.
 {RPC r;r.session=true;assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_SUCCESS);
  assert(r.sessionAsked&&out.sessionMode==ComputeSessionMode::Legacy&&out.hostBase==r.base&&
         out.hostBytes==r.windowBytes&&r.reserve==1&&r.releases==1);}
 // A KFD process: after the probe's GART window, it reserves and sets its own
 // window of the size the driver offers, and releases the hint.
 {RPC r;r.session=true;r.kfd=true;assert(initializeShimDevice(r,out,claimed)==HSA_STATUS_SUCCESS);
  assert(out.sessionMode==ComputeSessionMode::KFD&&out.hostBase==r.kfdCandidate&&
         out.hostBytes==r.kfdWindowBytes&&r.kfdConfigs==1&&r.kfdReserves==1&&r.reserve==1&&
         r.releases==2&&out.capacity==(31ull<<30));}
 // Joining a ready device: only the KFD window.
 {RPC r;r.ready=true;r.session=true;r.kfd=true;
  assert(initializeShimDevice(r,out,claimed,false)==HSA_STATUS_SUCCESS);
  assert(out.sessionMode==ComputeSessionMode::KFD&&!r.reserve&&r.kfdReserves==1&&r.releases==1);}
 // A window the driver already holds for this client is kept.
 {RPC r;r.ready=true;r.session=true;r.kfd=true;r.kfdBase=r.kfdCandidate;
  assert(initializeShimDevice(r,out,claimed,false)==HSA_STATUS_SUCCESS);
  assert(out.hostBase==r.kfdCandidate&&!r.kfdReserves&&!r.kfdConfigs);}
 // The window reservation can fail like the GART one, and is reported as such.
 {RPC r;r.ready=true;r.session=true;r.kfd=true;r.failKFDReserve=true;ShimInitializationFailure failure;
  assert(initializeShimDevice(r,out,claimed,false,&failure)==HSA_STATUS_ERROR_OUT_OF_RESOURCES);
  assert(failure.operation==ShimInitializationOperation::ReserveHostWindow&&!r.kfdConfigs&&!out.capacity);}
 // A window size the runtime cannot reserve is refused before reserving.
 {RPC r;r.ready=true;r.session=true;r.kfd=true;r.kfdWindowBytes=(1ull<<37)+16384;
  assert(initializeShimDevice(r,out,claimed,false)==HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
  assert(!r.kfdReserves);}
 // A driver with the tag that gives this client no session fails the
 // initialization there, never a session without queues.
 for(bool fresh:{false,true}){
  RPC r;r.ready=fresh?false:true;r.session=true;r.sessionRefused=true;ShimInitializationFailure failure;
  assert(initializeShimDevice(r,out,claimed,fresh,&failure)==HSA_STATUS_ERROR_OUT_OF_RESOURCES);
  assert(failure.operation==ShimInitializationOperation::Scalar&&failure.selector==21&&
         failure.hasTag&&failure.tag==kComputeSessionQueryTag&&!out.capacity);
  std::string text;
  reportShimInitializationFailure(failure,HSA_STATUS_ERROR_OUT_OF_RESOURCES,
                                  [&](std::string_view t){text+=t;});
  assert(text.find("no compute session")!=std::string::npos);}
 {RPC r;r.ready=true;r.session=true;r.sessionNoMode=true;ShimInitializationFailure failure;
  assert(initializeShimDevice(r,out,claimed,false,&failure)==HSA_STATUS_ERROR_INCOMPATIBLE_ARGUMENTS);
  assert(failure.operation==ShimInitializationOperation::Validation&&failure.tag==kComputeSessionQueryTag);}
 std::puts("Shim init fails when the driver gives the client no compute session");
 std::puts("Shim init negotiates the compute session: legacy keeps the GART window, a KFD process sets its own");
 std::puts("Shim init runs the firmware servicer only around InitDevice and stops it on every exit");
 std::puts("Shim HSA claim-before-configure, ready reuse, failure unwind and capability gates passed");
}
