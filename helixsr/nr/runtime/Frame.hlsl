// SPDX-License-Identifier: MIT
// Proxy feature/noise ordering and temporal blend derived from the pinned MIT
// OpenDLSS-NR frame.wgsl, commit9d08f4184bbcb9d858e2fb7a7834ec0837a9d2f1.
#include "FrameConstants.h"
ConstantBuffer<NrFrameConstants> P : register(b0);
Texture2D<float4> Color : register(t0);
Texture2D<float2> Motion : register(t1);
Texture2D<float> Depth : register(t2);
Texture2D<float4> History : register(t3);
Texture2D<float> HistoryDepth : register(t4);
ByteAddressBuffer Head : register(t5);
RWByteAddressBuffer Features : register(u0);
RWTexture2D<float4> Output : register(u1);
RWTexture2D<float4> NextHistory : register(u2);
RWTexture2D<float> NextDepth : register(u3);

uint rne(uint value,uint shift) {
    if (shift==0) return value;
    if (shift>31) return 0;
    uint q=value>>shift,r=value&((1u<<shift)-1u),mid=1u<<(shift-1);
    return q+uint(r>mid||(r==mid&&(q&1u)!=0));
}
uint half_bits(float value) {
    uint bits=asuint(value),sign=(bits>>16)&0x8000u,e=(bits>>23)&255u,m=bits&0x7fffffu;
    if(e==255)return sign|(m!=0?0x7e00u:0x7c00u);
    int he=int(e)-112;
    if(he>=31)return sign|0x7c00u;
    if(he<=0){if(he< -10)return sign;return sign|rne(m|0x800000u,uint(14-he));}
    uint rounded=rne(m,13);
    if(rounded==1024u){rounded=0;he++;}
    return sign|(he>=31?0x7c00u:(uint(he)<<10)|rounded);
}
float from_half(uint h) {
    uint sign=(h&0x8000u)<<16,e=(h>>10)&31u,m=h&1023u;
    if(e==0){
        if(m==0)return asfloat(sign);
        int shift=0;while((m&1024u)==0){m<<=1;shift++;}
        return asfloat(sign|(uint(113-shift)<<23)|((m&1023u)<<13));
    }
    if(e==31)return asfloat(sign|0x7f800000u|(m<<13));
    return asfloat(sign|((e+112u)<<23)|(m<<13));
}
float round_half(float x){return from_half(half_bits(x));}
float3 round_half3(float3 x){return float3(round_half(x.x),round_half(x.y),round_half(x.z));}
float centre(float x){return round_half(round_half(round_half(x)-.5)*.125);}
float3 centre3(float3 x){return float3(centre(x.x),centre(x.y),centre(x.z));}
float truncate_half(float value) {
    uint bits=asuint(value),sign=(bits>>16)&0x8000u,e=(bits>>23)&255u,m=bits&0x7fffffu;
    int he=int(e)-112;
    if(e==255)return from_half(sign|(m!=0?0x7e00u:0x7c00u));
    if(he>=31)return from_half(sign|0x7c00u);
    if(he<=0)return from_half(he< -10?sign:sign|((m|0x800000u)>>uint(14-he)));
    return from_half(sign|(uint(he)<<10)|(m>>13));
}
float3 truncate_half3(float3 x){return float3(truncate_half(x.x),truncate_half(x.y),truncate_half(x.z));}
float srgb_encode(float x){x=saturate(x);return x<=.0031308?12.92*x:1.055*pow(x,1.0/2.4)-.055;}
float srgb_decode(float x){x=saturate(x);return x<=.04045?x/12.92:pow((x+.055)/1.055,2.4);}
float3 decode3(float3 x){return float3(srgb_decode(x.r),srgb_decode(x.g),srgb_decode(x.b));}
float proxy_component(float x){
    x=isfinite(x)?max(x,0):0;
    if(P.colorSpace==1){x/=P.paperWhite;if(x>.75)x=.75+.25*(1-exp(-5.770780*(x-.75)));}
    return round_half(srgb_encode(x));
}
float3 proxy3(float3 x){return P.colorSpace==0?x:float3(proxy_component(x.r),proxy_component(x.g),proxy_component(x.b));}
float3 to_oklab(float3 c){
    float3 l=float3(dot(float3(.4122214708,.5363325363,.0514459929),c),dot(float3(.2119034982,.6806995451,.1073969566),c),dot(float3(.0883024619,.2817188376,.6299787005),c));
    l=sign(l)*pow(abs(l),1.0/3.0);
    return float3(dot(float3(.2104542553,.7936177850,-.0040720468),l),dot(float3(1.9779984951,-2.4285922050,.4505937099),l),dot(float3(.0259040371,.7827717662,-.8086757660),l));
}
float3 from_oklab(float3 c){
    float3 l=float3(dot(float3(1,.3963377774,.2158037573),c),dot(float3(1,-.1055613458,-.0638541728),c),dot(float3(1,-.0894841775,-1.2914855480),c));l=l*l*l;
    return float3(dot(float3(4.0767416621,-3.3077115913,.2309699292),l),dot(float3(-1.2684380046,2.6097574011,-.3413193965),l),dot(float3(-.0041960863,-.7034186147,1.7076147010),l));
}
float3 clamp_ap1(float3 c){
    float3 a=max(float3(dot(float3(.613097,.339523,.047379),c),dot(float3(.070194,.916354,.013452),c),dot(float3(.020616,.109570,.869815),c)),0);
    return float3(dot(float3(1.705051,-.621792,-.083259),a),dot(float3(-.130256,1.140805,-.010548),a),dot(float3(-.024003,-.128969,1.152972),a));
}
float luminance(float3 c){return dot(c,float3(.212639,.715169,.072192));}
float3 upgrade_tone(float3 original,float3 proxy,float3 neural){
    float a=luminance(original),b=luminance(proxy),c=luminance(neural);if(c<=.00001)return original;
    float ratio=a<b?a/max(b,.000001):(c+max(0,a-b))/c;
    float3 lab=to_oklab(neural*ratio),correct=to_oklab(neural);float chroma=length(correct.yz);
    float scale=chroma==0?1:length(lab.yz)/chroma;lab.yz=correct.yz*scale;
    return clamp_ap1(from_oklab(lab));
}
// The pinned MIT frame pipeline separates neural luminance from color transfer.
// A neutral-style head can remove scene chroma: preserving RGB proportions here
// retains the game's color while still applying the neural light/detail result.
float3 transfer_color(float3 original,float3 corrected){
    float sourceY=luminance(original);
    float gain=sourceY<=.000001?1:clamp(luminance(corrected)/sourceY,0,4);
    float3 sourceColor=original*gain;
    return lerp(sourceColor,corrected,P.colorStrength);
}
uint mirror(uint x,uint width) {
    // Periodic reflection also defines unusually narrow aspect ratios, without
    // unsigned-underflow reads outside the source rectangle.
    if(width<=1)return 0;
    uint period=2u*width-2u,q=x%period;
    return q<width?q:period-q;
}
float hash_uniform(uint x){x=(x>>((x>>28)+4))^x;x*=0x108ef2d9u;return float(((x>>30)^(x>>8))+1u)*asfloat(0x33800000u);}
float3 gaussian(uint2 xy) {
    uint b=(xy.x*0x8da6b343u)^(P.seed*0x9e3779b9u)^(xy.y*0xd8163841u)^0x243f6a88u;
    b=(b>>((b>>28)+4))^b;b*=0x108ef2d9u;b=(b>>22)^b;
    float u0=hash_uniform(b*0x2c9277b5u+0xac564b05u),u1=hash_uniform(b*0xfa6dc5f9u+0x4712a88eu);
    float u2=hash_uniform(b*0xcaa5b80du+0x21dd796bu),u3=hash_uniform(b*0x83232c31u+0x3463e0acu);
    precise float r0=sqrt(log2(u0)*asfloat(0x3f317218u)*-2.0),r1=sqrt(log2(u2)*asfloat(0x3f317218u)*-2.0);
    float a0=u1*asfloat(0x40c90fdbu),a1=u3*asfloat(0x40c90fdbu);
    return round_half3(float3(r0*cos(a0),r0*sin(a0),r1*cos(a1)));
}
float3 color_at(float2 uv) {
    float2 p=uv*float2(P.colorSize)-.5,f=frac(p);int2 b=int2(floor(p));
    int2 hi=int2(P.colorSize)-1,lo=int2(P.colorBase);
    float3 a=Color.Load(int3(clamp(b,0,hi)+lo,0)).rgb;
    float3 c=Color.Load(int3(clamp(b+int2(1,0),0,hi)+lo,0)).rgb;
    float3 d=Color.Load(int3(clamp(b+int2(0,1),0,hi)+lo,0)).rgb;
    float3 e=Color.Load(int3(clamp(b+1,0,hi)+lo,0)).rgb;
    return lerp(lerp(a,c,f.x),lerp(d,e,f.x),f.y);
}
float2 displacement(float2 uv) {
    uint2 pixel=min(uint2(uv*P.motionSize),P.motionSize-1u)+P.motionBase;
    return Motion.Load(int3(pixel,0))*P.motionScale;
}
float current_depth(float2 uv) {
    if(any(P.depthSize==0))return 0;
    uint2 pixel=min(uint2(uv*P.depthSize),P.depthSize-1u)+P.depthBase;
    return Depth.Load(int3(pixel,0));
}
float3 history_bilinear(float2 uv) {
    float2 p=uv*P.displaySize-.5,f=frac(p);int2 b=int2(floor(p)),hi=int2(P.displaySize)-1;
    return lerp(lerp(History.Load(int3(clamp(b,0,hi),0)).rgb,History.Load(int3(clamp(b+int2(1,0),0,hi),0)).rgb,f.x),
                lerp(History.Load(int3(clamp(b+int2(0,1),0,hi),0)).rgb,History.Load(int3(clamp(b+1,0,hi),0)).rgb,f.x),f.y);
}
float3 reproject_history(float2 uv) {
    float2 size=float2(P.displaySize),position=(uv+displacement(uv))*size;
    float2 b=floor(position-.5)+.5,f=saturate(position-b),s=f*f,c=f*s;
    float2 w0=s-.5*(f+c),w1=(1.5*c-2.5*s)+1,w3=(c-s)*.5,w2=((1-w0)-w1)-w3,m=w1+w2;
    float2 low=clamp(b-1,.5,size-.5)/size,mid=clamp(b+w2/m,.5,size-.5)/size,high=clamp(b+2,.5,size-.5)/size;
    float a=w0.x*m.y,bb=w0.y*m.x,cc=m.x*m.y,d=w3.y*m.x,e=w3.x*m.y;
    precise float3 value=history_bilinear(float2(low.x,mid.y))*a+history_bilinear(float2(mid.x,low.y))*bb;
    value=history_bilinear(mid)*cc+value;value=history_bilinear(float2(mid.x,high.y))*d+value;
    value=history_bilinear(float2(high.x,mid.y))*e+value;
    return value/(e+(d+(cc+(a+bb))));
}
bool history_allowed(float2 uv) {
    if(P.historyValid==0||any(P.motionSize==0))return false;
    float2 previous=uv+displacement(uv);
    if(any(previous<0)||any(previous>1))return false;
    if(all(P.depthSize>0)){
        int2 pixel=clamp(int2(previous*P.displaySize),0,int2(P.displaySize)-1);
        float a=current_depth(uv),b=HistoryDepth.Load(int3(pixel,0));
        if(!isfinite(a)||!isfinite(b)||abs(a-b)>P.depthThreshold*max(max(abs(a),abs(b)),.001))return false;
    }
    return true;
}
[numthreads(8,8,1)]
void FramePre(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=P.fieldSize))return;
    uint2 pixel=uint2(mirror(id.x,P.networkSize.x),mirror(id.y,P.networkSize.y));
    float2 uv=(float2(pixel)+.5)/float2(P.networkSize);
    float3 code=all(P.colorSize==P.networkSize)?Color.Load(int3(pixel+P.colorBase,0)).rgb:color_at(uv);
    float3 current=centre3(proxy3(code)),previous=current;
    if(history_allowed(uv))previous=centre3(reproject_history(uv));
    float3 noise=gaussian(id.xy);uint base=(id.y*P.fieldSize.x+id.x)*64u;
    Features.Store4(base,asuint(float4(noise,1)));
    Features.Store3(base+16,asuint(current));Features.Store3(base+28,asuint(previous));
    Features.Store(base+40,asuint(P.style/128.0));Features.Store(base+44,asuint(round_half(P.localTone)));
    float skin=P.skinStructure<0?P.localStructure:P.skinStructure;
    Features.Store(base+48,asuint(round_half(P.autoMask!=0?1:P.localStructure)));
    Features.Store(base+52,asuint(round_half(P.autoMask!=0?skin:-1)));
    Features.Store(base+56,asuint(round_half(P.autoMask!=0?P.localStructure:-1)));
    Features.Store(base+60,0);
}
float4 head_at(float2 uv) {
    float2 p=uv*P.networkSize-.5,f=frac(p);int2 b=int2(floor(p)),hi=int2(P.networkSize)-1;
    uint2 a=uint2(clamp(b,0,hi)),c=uint2(clamp(b+int2(1,0),0,hi));
    uint2 d=uint2(clamp(b+int2(0,1),0,hi)),e=uint2(clamp(b+1,0,hi));
    return lerp(lerp(asfloat(Head.Load4((a.y*P.fieldSize.x+a.x)*16)),asfloat(Head.Load4((c.y*P.fieldSize.x+c.x)*16)),f.x),
                lerp(asfloat(Head.Load4((d.y*P.fieldSize.x+d.x)*16)),asfloat(Head.Load4((e.y*P.fieldSize.x+e.x)*16)),f.x),f.y);
}
[numthreads(8,8,1)]
void FramePost(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=P.displaySize))return;
    float2 uv=(float2(id.xy)+.5)/P.displaySize;
    float3 scene=all(P.colorSize==P.displaySize)?Color.Load(int3(id.xy+P.colorBase,0)).rgb:color_at(uv);
    float3 code=proxy3(scene);
    float4 head=all(P.networkSize==P.displaySize)?asfloat(Head.Load4((id.y*P.fieldSize.x+id.x)*16)):head_at(uv);
    precise float3 inner=head.rgb*.03125+(code*.125-.0625);
    float3 neural=saturate(inner*8+.5);
    if(history_allowed(uv)){
        float weight=saturate(1.0/(1.0+exp2(head.a*-1.4426950408889634))*P.historyBlend);
        neural=lerp(neural,reproject_history(uv),weight);
    }
    float3 stored=truncate_half3(neural);
    float3 published=P.intensity==1?stored:truncate_half3(saturate(lerp(code,neural,P.intensity)));
    if(P.colorSpace!=0){
        float3 corrected=P.colorSpace==1?upgrade_tone(scene,decode3(code),decode3(stored)):decode3(stored);
        if(P.colorStrength<1&&P.intensity>0)corrected=transfer_color(scene,corrected);
        published=P.intensity==0?scene:clamp(lerp(scene,corrected,P.intensity),-65504,65504);
    }else if(P.colorStrength<1&&P.intensity>0){
        float3 corrected=transfer_color(decode3(code),decode3(published));
        published=float3(srgb_encode(corrected.r),srgb_encode(corrected.g),srgb_encode(corrected.b));
    }
    Output[id.xy]=float4(published,1);NextHistory[id.xy]=float4(stored,1);
    NextDepth[id.xy]=current_depth(uv);
}
