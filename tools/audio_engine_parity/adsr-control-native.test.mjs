import {mkdtempSync, readFileSync, writeFileSync} from 'node:fs';
import {spawnSync} from 'node:child_process';
import os from 'node:os';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import assert from 'node:assert/strict';

const root = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '../..');
const source = file => readFileSync(path.join(root, 'main/audio/engine', file), 'utf8');
function declaration(text, signature) {
 const start = text.indexOf(signature);
 if (start < 0) throw new Error(signature);
 let end = text.indexOf('{', start), depth = 1;
 while (depth && ++end < text.length) { if (text[end] === '{') ++depth; if (text[end] === '}') --depth; }
 return text.slice(start, end + 1);
}
const adsr = source('nodes/AdsrNode.h');
const envelope = source('nodes/DxEnvelopeNode.h');
const envelopeActivity = source('EnvelopeActivity.h');
const synthEngine = source('SynthEngine.h');
assert.match(synthEngine,/publishEnvelopeActivity\(target.targetId, target.paramName\)/);
assert.match(synthEngine,/publishEnvelopeActivity\(widgetId->valuestring, parameter->string\)/);
const oscillator = source('nodes/FmOscillatorNode.h');
assert.match(source('Graph.h'), /nodes::adsr::updateParameters\(target\);\s*target.adsr->ramp.gate\(noteOn\)/);
assert.match(source('Graph.h'), /nodes::dxenvelope::updateParameters\(target\);\s*noteOn \? target.dxEnvelope->scheduler.gateOn/);
const directory = mkdtempSync(path.join(os.tmpdir(), 'espressynth-adsr-control-'));
writeFileSync(path.join(directory, 'test.cpp'), `
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <vector>
#include "${path.join(root, "main/features/synth/KnobActivity.h").replaceAll("\\", "/")}"
namespace synth {using PortMask=uint32_t;constexpr int kSampleRate=1000;inline uint32_t portBit(int p){return 1u<<p;}}
namespace enginestats {struct {unsigned dxShapeConfigured=0,dxHolding=0,dxRamping=0,dxBoundary=0;} current;}
float clampf(float value,float lo,float hi){return std::fmax(lo,std::fmin(hi,value));}
namespace cv {
constexpr float kMaxVolts=10,kInvMaxVolts=0.1f,kGateThresholdVolts=5,kAudioPeakVolts=5;
float vcaGain(float v,bool){return clampf(v,0,10)*kInvMaxVolts;}
float hzFromPitchCv(float){return 100;}
${declaration(source('CvConventions.h'), 'inline float cvToEnvelopeSeconds(')}
}
${declaration(source('DspPrimitives.h'), 'class RampScheduler')};
namespace nodes::adsr {${declaration(adsr, 'struct AdsrState')};}
${envelope.slice(envelope.indexOf('namespace nodes::dxenvelope'), envelope.indexOf('inline void updateParameters('))}}
enum class NodeType {Adsr,DxEnvelope,Knob};
struct NodeInstance {
 NodeType type=NodeType::Adsr;
 nodes::adsr::AdsrState *adsr=nullptr;
 nodes::dxenvelope::DxEnvelopeState *dxEnvelope=nullptr;
 uint32_t audioConnectedPortMask=0,controlConnectedPortMask=0;
 float controlValue[10]={},*inPorts[10]={},*outL=nullptr;
 bool outputMetadataOnly=false;
 bool outIsRamp=false,outIsConstant=false;float outConstantValue=0;
 struct {float start=0,inc=0;} outRamp;
};
namespace noderender {
${declaration(source('NodeInstance.h'), 'inline float portSample(')}
${declaration(source('NodeInstance.h'), 'inline bool portIsFed(')}
${declaration(source('NodeInstance.h'), 'inline float clampToRange(')}
}
namespace nodes::adsr {
${declaration(adsr, 'inline void updateParameters(')}
${declaration(adsr, 'inline void render(')}
${declaration(adsr, 'inline bool setParam(')}
}
namespace nodes::dxenvelope {
${declaration(envelope, 'inline void updateParameters(')}
${declaration(envelope, 'inline void render(')}
${declaration(envelope, 'inline int parameterIndex(')}
${declaration(envelope, 'inline bool setParam(')}
}
${declaration(envelopeActivity, 'inline bool isEnvelopeShapeParameter(')}
${declaration(envelopeActivity, 'inline void fillEnvelopeActivity(')}
struct WidgetSlot {NodeType type=NodeType::Adsr;char id[24]={},label[48]={};uint16_t instanceBase=0;};
struct Graph {uint16_t widgetCount=0;WidgetSlot widgets[3];NodeInstance instances[3];};
std::vector<KnobActivity> notifications;
constexpr int pdTRUE=1;
int64_t esp_timer_get_time(){return 123456;}
int xQueueSend(void *,const KnobActivity *a,int){notifications.push_back(*a);return pdTRUE;}
int xQueueReceive(void *,KnobActivity *,int){return 0;}
struct ActivityPublisher {
 Graph *liveGraph_=nullptr;void *knobActivityQueue_=this;
 uint32_t presetChangeSequence(){return 7;}
 ${declaration(synthEngine, '    bool publishEnvelopeActivity(')}
};
#define IRAM_ATTR
struct PhaseOsc {
 uint32_t phase=0;
 uint32_t phaseInt(){return phase;}
 static uint32_t toPhaseInt(float){return 1;}
 static uint32_t toPhaseOffsetInt(float){return 0;}
 static float sawSampleAt(uint32_t){return 1;}
 void advancePhaseOnly(uint32_t increment,size_t frames){phase+=increment*frames;}
};
struct FmOscillatorState {
 PhaseOsc phaseGen;uint32_t superSawSidePhase[6]={};float feedbackSample=0;
 struct {float process(float value){return value;}} gainSmooth;
};
namespace nodes::fmoscillator {
${declaration(oscillator, 'struct SuperSawLoopCtx')};
template <bool AudioRatePitch, bool ConstantOutput>
${declaration(oscillator, 'IRAM_ATTR inline void renderSuperSawLoop(')}
}
void near(float actual,float expected){assert(std::abs(actual-expected)<0.00001f);}
int main(){
 nodes::adsr::AdsrState state;NodeInstance inst;inst.adsr=&state;float output[128];inst.outL=output;
 state.ramp.configure(0.1f,0.2f,0.3f,0.4f,1000);
 // Knob/Button/Const routes seed and update controlValue, without DSP input buffers.
 inst.controlConnectedPortMask=15;
 inst.controlValue[0]=2.5f;inst.controlValue[1]=5;inst.controlValue[2]=7;inst.controlValue[3]=10;
 nodes::adsr::render(inst,1);
 near(state.ramp.attackSec(),1);near(state.ramp.decaySec(),2);
 near(state.ramp.sustainLevel(),0.7f);near(state.ramp.releaseSec(),4);
 state.ramp.gate(true);nodes::adsr::render(inst,1);near(output[0],0.01f);
 inst.controlValue[0]=5;inst.controlValue[1]=2.5f;inst.controlValue[2]=2;inst.controlValue[3]=5;
 nodes::adsr::render(inst,1);
 near(state.ramp.attackSec(),2);near(state.ramp.decaySec(),1);
 near(state.ramp.sustainLevel(),0.2f);near(state.ramp.releaseSec(),2);
 // A subsequent note uses the updated attack rather than the original slider value.
 nodes::adsr::AdsrState fresh;inst.adsr=&fresh;
 fresh.ramp.configure(0.1f,0.1f,0.7f,0.2f,1000);
 nodes::adsr::updateParameters(inst); // MIDI note-on after an idle control update.
 fresh.ramp.gate(true);nodes::adsr::render(inst,1);near(output[0],0.005f);
 // DSP and scalar control inputs can feed different ADSR parameters together.
 float decay[1]={10};inst.inPorts[1]=decay;inst.audioConnectedPortMask=2;inst.controlConnectedPortMask=13;
 fresh.attackLog=true;nodes::adsr::render(inst,1);
 near(fresh.ramp.attackSec(),4*std::pow(10.0f,-1.5f));near(fresh.ramp.decaySec(),4);
 // Disconnecting preserves the direct seconds/level slider path.
 inst.controlConnectedPortMask=0;inst.audioConnectedPortMask=0;
 assert(nodes::adsr::setParam(inst,"attack",0.25f));assert(nodes::adsr::setParam(inst,"sustain",0.6f));
 nodes::adsr::render(inst,1);near(fresh.ramp.attackSec(),0.25f);near(fresh.ramp.sustainLevel(),0.6f);
 puts("PASS ADSR: all four scalar CV controls, live changes, audible attack slope, mixed DSP/control, logarithmic mapping and direct sliders");
 nodes::dxenvelope::DxEnvelopeState shape;inst.dxEnvelope=&shape;inst.inPorts[1]=nullptr;
 inst.controlConnectedPortMask=255;
 for(int i=0;i<4;++i){inst.controlValue[i]=static_cast<float>(8-i*2);inst.controlValue[i+4]=static_cast<float>(2.5f*(i+1));}
 nodes::dxenvelope::render(inst,1);
 for(int i=0;i<4;++i){near(shape.scheduler.level(i),8-i*2);near(shape.scheduler.time(i),i+1);}
 inst.controlValue[0]=10;inst.controlValue[3]=0;inst.controlValue[4]=5;
 // The raw setParam call from the control route must not leave volts interpreted as seconds.
 assert(nodes::dxenvelope::setParam(inst,"time1",5));
 nodes::dxenvelope::updateParameters(inst);near(shape.scheduler.time(0),2);
 shape.scheduler.gateOn();nodes::dxenvelope::render(inst,1);near(output[0],0.005f);
 inst.controlValue[4]=2.5f;inst.controlValue[7]=5;
 nodes::dxenvelope::updateParameters(inst);near(shape.scheduler.time(0),1);near(shape.scheduler.time(3),2);
 shape.timeLog[1]=true;nodes::dxenvelope::render(inst,1);near(shape.scheduler.time(1),4*std::pow(10.0f,-1.5f));
 inst.controlConnectedPortMask=0;
 assert(nodes::dxenvelope::setParam(inst,"time1",0.25f));assert(nodes::dxenvelope::setParam(inst,"level2",6));
 nodes::dxenvelope::render(inst,1);near(shape.scheduler.time(0),0.25f);near(shape.scheduler.level(1),6);
 // Real Envelope metadata skips buffer writes; Supersaw must consume the ramp.
 inst.outputMetadataOnly=true;shape.scheduler.gateOn();
 for(float &sample:output) sample=123;
 nodes::dxenvelope::render(inst,16);assert(inst.outIsRamp);near(output[0],123);
 FmOscillatorState saw;float samples[128],stale[128];for(float &value:stale)value=0;
 nodes::fmoscillator::SuperSawLoopCtx ctx{};
 ctx.inst=&inst;ctx.state=&saw;ctx.ampBuf=stale;ctx.centerWeight=1;
 ctx.gainConverged=true;ctx.gainTarget=1;ctx.hasAmpRamp=true;
 ctx.ampRampStart=inst.outRamp.start*0.1f;ctx.ampRampStep=inst.outRamp.inc*0.1f;
 inst.outL=samples;
 nodes::fmoscillator::renderSuperSawLoop<false,false>(ctx,16);
 for(int i=0;i<16;++i)near(samples[i],(inst.outRamp.start+inst.outRamp.inc*i)*0.5f);
 ctx.ampRampStart=1;ctx.ampRampStep=-0.05f;ctx.gainConverged=false;
 nodes::fmoscillator::renderSuperSawLoop<true,false>(ctx,16);
 for(int i=0;i<16;++i)near(samples[i],(1-0.05f*i)*5);
 ctx.hasAmpRamp=false;ctx.ampConstantForBlock=true;ctx.ampConstantValue=0.4f;
 nodes::fmoscillator::renderSuperSawLoop<false,false>(ctx,16);near(samples[15],2);
 ctx.ampConstantValue=0;nodes::fmoscillator::renderSuperSawLoop<false,false>(ctx,16);near(samples[15],0);
 // Effective curve values must follow scalar controls even while the voice is idle.
 inst.type=NodeType::Adsr;inst.adsr=&fresh;inst.controlConnectedPortMask=15;inst.audioConnectedPortMask=0;
 inst.controlValue[0]=5;inst.controlValue[1]=2.5f;inst.controlValue[2]=7;inst.controlValue[3]=10;
 fresh.attackLog=false;KnobActivity curve;fillEnvelopeActivity(curve,inst);
 assert(curve.isEnvelope && curve.durationUs()==2000000);
 assert(curve.envelope[1].x==153 && curve.envelope[1].y==1000);
 assert(curve.envelope[2].x==230 && curve.envelope[2].y==700);
 assert(curve.envelope[3].x==312 && curve.envelope[4].x==618 && curve.envelope[4].y==0);
 inst.type=NodeType::DxEnvelope;inst.controlConnectedPortMask=255;
 for(int i=0;i<4;++i){inst.controlValue[i]=8-i*2;inst.controlValue[i+4]=5;}
 shape.timeLog[1]=false;fillEnvelopeActivity(curve,inst);
 assert(curve.envelope[0].y==200 && curve.envelope[1].y==800 && curve.envelope[3].y==400 && curve.envelope[5].y==200);
 assert(curve.envelope[4].x-curve.envelope[3].x==82);
 Graph graph;graph.widgetCount=2;graph.instances[0]=inst;graph.instances[1]=inst;
 for(int i=0;i<2;++i){graph.widgets[i].type=NodeType::DxEnvelope;graph.widgets[i].instanceBase=i;snprintf(graph.widgets[i].id,24,"env-%d",i);snprintf(graph.widgets[i].label,48,"Envelope %d",i);}
 ActivityPublisher publisher;publisher.liveGraph_=&graph;
 assert(!publisher.publishEnvelopeActivity("missing","time1"));
 assert(!publisher.publishEnvelopeActivity("env-0","gate"));
 assert(!publisher.publishEnvelopeActivity("env-0","velocity"));
 assert(publisher.publishEnvelopeActivity("env-0","time1"));
 assert(publisher.publishEnvelopeActivity("env-1","level3"));
 assert(notifications.size()==2 && strcmp(notifications[0].name,"Envelope 0")==0 && strcmp(notifications[1].sourceId,"env-1")==0);
 assert(notifications[0].presetSequence==7 && notifications[0].updatedUs==123456);
 assert(isEnvelopeShapeParameter(NodeType::Adsr,"att") && isEnvelopeShapeParameter(NodeType::Adsr,"dec") && isEnvelopeShapeParameter(NodeType::Adsr,"sus") && isEnvelopeShapeParameter(NodeType::Adsr,"rel"));
 assert(isEnvelopeShapeParameter(NodeType::Adsr,"attackMaxSeconds") && isEnvelopeShapeParameter(NodeType::DxEnvelope,"time2Curve"));
 puts("PASS Envelope activity: effective idle CV values, web-style ADSR/Envelope geometry, names, concurrent targets and no gate/velocity popup");
 puts("PASS Supersaw: real Envelope metadata with stale buffer, rising/falling ramps, audio-rate pitch, gain smoothing, held and silent amplitude");
 puts("PASS Envelope: eight scalar CV inputs, live changes, correct volts/time mapping, first-note slope, logarithmic mapping and direct sliders");
}
`);
function run(command,args){
 const result=spawnSync(command,args,{cwd:directory,encoding:'utf8',windowsVerbatimArguments:command==='cmd.exe',timeout:30000});
 if(result.error)throw result.error;
 if(result.status!==0)throw new Error(result.stdout+result.stderr);
 return result.stdout;
}
if(process.platform==='win32'){
 const vswhere=path.join(process.env['ProgramFiles(x86)'],'Microsoft Visual Studio/Installer/vswhere.exe');
 const installation=run(vswhere,['-latest','-products','*','-requires','Microsoft.VisualStudio.Component.VC.Tools.x86.x64','-property','installationPath']).trim();
 const vcvars=path.join(installation,'VC/Auxiliary/Build/vcvars64.bat');
 run('cmd.exe',['/d','/c',`call "${vcvars}" >nul && cl /nologo /EHsc /std:c++17 /Fe:test.exe test.cpp`]);
 console.log(run(path.join(directory,'test.exe'),[]).trim());
}else{
 run(process.env.CXX||'c++',['-std=c++17','test.cpp','-o','test']);
 console.log(run(path.join(directory,'test'),[]).trim());
}
