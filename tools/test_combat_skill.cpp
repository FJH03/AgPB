// Regression of the source difficulty boundaries and aim branches.
#include "../src/combat_skill.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>
static void Require(bool good, const char *why) {
    if (!good) { std::fprintf(stderr,"FAIL: %s\n",why); std::exit(1); }
}
static bool Near(float a,float b) { return std::fabs(a-b)<0.0001f; }
int main() {
    using namespace AgPBCombatSkill;
    const int lows[]={1,30,50,80,100}, highs[]={29,49,79,99,100};
    for(int d=0;d<5;++d) {
        const Range range=DifficultyRange(d);
        Require(range.minimum==lows[d] && range.maximum==highs[d],"five exact SyPB creation ranges");
        for(int skill=range.minimum;skill<=range.maximum;++skill)
            Require(skill>=1 && skill<=100,"all preset random outcomes are legal skills");
    }
    const int edges[]={1,19,20,39,40,59,60,79,80,99,100};
    const int buckets[]={0,0,1,1,2,2,3,3,4,4,5};
    for(int i=0;i<11;++i) Require(Bucket(edges[i])==buckets[i],"six internal skill/20 table rows");
    const float mins[]={.8f,.6f,.4f,.2f,.1f,0}, maxs[]={1,.8f,.6f,.3f,.2f,.1f};
    const int recoil[]={50,40,35,30,23,20};
    for(int i=0;i<6;++i) {
        const Row row=SkillRow(i==0?1:i*20);
        Require(Near(row.minReaction,mins[i])&&Near(row.maxReaction,maxs[i])&&row.recoilAmount==recoil[i],"exact reaction/recoil table");
    }
    Require(Near(ReactionDelay(.9f,80,79),.3f)&&Near(ReactionDelay(.9f,80,80),.9f),"source reaction chance strict boundary");
    Require(!WantsFire(false,1)&&!WantsFire(true,.799f)&&WantsFire(true,.80f),"surprise and source shooting cone");
    Require(!PerfectAim(69,true,false)&&PerfectAim(70,true,false)&&!PerfectAim(100,false,false),"normal attack snap begins at70, requires wantsFire");
    const View target={-25,-150}, current={0,0};
    const View strong=StepAim(current,target,0,0,1.0f/64,100,true,false,true,25,false);
    Require(Near(strong.pitch,target.pitch)&&Near(strong.yaw,target.yaw),"strongest source snap is not defeated by a global cap");
    const View weak=StepAim(current,target,0,0,1.0f/64,30,true,false,true,25,false);
    Require(std::fabs(weak.yaw)<std::fabs(target.yaw)&&std::fabs(weak.pitch)<std::fabs(target.pitch),"low skill uses proportional spring");
    Require(Near(Spring(99,true,0,false),4.96f)&&Near(Spring(100,true,0,false),11.25f),"source integer skill/100 bonus preserved");
    Require(HasAimNoise(50)&&!HasAimNoise(51),"bbox noise threshold");
    Require(AimAtHead(80,80,false,100)&&!AimAtHead(29,30,false,100)&&!AimAtHead(100,30,true,150),"source head choice and AWP exception");
    Require(Near(RelativeNoise(0,72,0),-36)&&Near(RelativeNoise(0,72,1),36),"Source geometry preserves centered bbox-sized error");
    Require(ImmediateRearRecognition(100)&&!ImmediateRearRecognition(99),"highest rear recognition exception");
    Require(!RecoilExceeded(1,0,0,3000)&&RecoilExceeded(100,5,0,3000),"source recoil distance gate");
    std::puts("combat skill tests passed");
}
