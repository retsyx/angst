// Render the native fetch sequence, its carried ball and moving paws through LVGL.
#include <lvgl.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
struct DogLine { lv_point_t from,to; };
static std::vector<DogLine> dog_lines;
static lv_area_t ball_bounds;
static bool observe_fetch,have_ball;
static void observedLine(lv_draw_ctx_t *context,const lv_draw_line_dsc_t *style,
                         const lv_point_t *from,const lv_point_t *to) {
    if(observe_fetch&&style->color.full==lv_color_hex(0xdca14b).full&&dog_lines.size()<32)
        dog_lines.push_back({*from,*to});
    lv_draw_line(context,style,from,to);
}
static void observedRect(lv_draw_ctx_t *context,const lv_draw_rect_dsc_t *style,const lv_area_t *area) {
    if(observe_fetch&&!have_ball&&style->bg_color.full==lv_color_hex(0xf16d87).full&&style->bg_opa>0) {
        ball_bounds=*area;have_ball=true;
    }
    lv_draw_rect(context,style,area);
}
#include "odometer.h"
#define private public
#define lv_draw_line observedLine
#define lv_draw_rect observedRect
#include "stick_figure.h"
#undef lv_draw_rect
#undef lv_draw_line
#undef private
LV_FONT_DECLARE(bodoni_72_smallcaps);
static constexpr int Width=536,Height=240;
static unsigned checks;
static const char *preview_directory;
struct Display {
    std::vector<lv_color_t> pixels=std::vector<lv_color_t>(Width*Height);
    std::vector<lv_color_t> buffer=std::vector<lv_color_t>(Width*16);
    lv_disp_draw_buf_t draw_buffer{};lv_disp_drv_t driver{};lv_disp_t *display;
    Display() {
        lv_disp_draw_buf_init(&draw_buffer,buffer.data(),nullptr,buffer.size());lv_disp_drv_init(&driver);
        driver.hor_res=Width;driver.ver_res=Height;driver.draw_buf=&draw_buffer;driver.user_data=this;
        driver.flush_cb=[](lv_disp_drv_t *driver,const lv_area_t *area,lv_color_t *colors) {
            auto *self=static_cast<Display *>(driver->user_data);
            for(int y=area->y1;y<=area->y2;++y)for(int x=area->x1;x<=area->x2;++x) {
                assert(x>=0&&x<Width&&y>=0&&y<Height);self->pixels[y*Width+x]=*colors++;
            }
            lv_disp_flush_ready(driver);
        };
        display=lv_disp_drv_register(&driver);assert(display);
        lv_obj_remove_style_all(root());lv_obj_clear_flag(root(),LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(root(),lv_color_black(),0);lv_obj_set_style_bg_opa(root(),LV_OPA_COVER,0);
    }
    ~Display() {
        lv_disp_remove(display);
        // LVGL 8.4 leaves the driver's software draw context for its owner to free.
        lv_mem_free(driver.draw_ctx);
        driver.draw_ctx = nullptr;
    }
    lv_obj_t *root(){return lv_disp_get_scr_act(display);}
    void render(){lv_obj_invalidate(root());lv_refr_now(display);assert(lv_mem_test()==LV_RES_OK);}
    void save(const std::string &name) {
        if(!preview_directory)return;
        std::ofstream output(std::string(preview_directory)+"/"+name+".ppm",std::ios::binary);
        assert(output);output<<"P6\n"<<Width<<' '<<Height<<"\n255\n";
        for(auto pixel:pixels){lv_color32_t rgb;rgb.full=lv_color_to32(pixel);
            const char data[]={static_cast<char>(rgb.ch.red),static_cast<char>(rgb.ch.green),static_cast<char>(rgb.ch.blue)};
            output.write(data,3);}
    }
};
static uint64_t frameHash(const Display &display) {
    uint64_t result=1469598103934665603ULL;for(auto pixel:display.pixels)result=(result^pixel.full)*1099511628211ULL;return result;
}

struct FetchCanvas {
    Display display;lv_obj_t *canvas;StickScenes::Frame frame={};
    explicit FetchCanvas(int facing) {
        frame.id=ANIMATION_DOG;frame.duration=animation_catalog_lookup(ANIMATION_DOG)->duration_ms;
        frame.x=facing>0?50:Width-50;frame.y=Height-10;frame.facing=facing;frame.scaleQ8=358;
        frame.width=Width;frame.height=Height;frame.opacity=frame.propOpacity=LV_OPA_COVER;
        canvas=lv_obj_create(display.root());lv_obj_remove_style_all(canvas);lv_obj_clear_flag(canvas,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(canvas,Width,Height);
        lv_obj_add_event_cb(canvas,[](lv_event_t *event){auto *self=static_cast<FetchCanvas *>(lv_event_get_user_data(event));
            lv_area_t bounds;lv_obj_get_coords(lv_event_get_target(event),&bounds);
            StickScenes::draw(lv_event_get_draw_ctx(event),bounds,self->frame);
        },LV_EVENT_DRAW_MAIN,this);
    }
    std::vector<int> at(uint32_t elapsed) {
        frame.elapsed=elapsed;dog_lines.clear();have_ball=false;observe_fetch=true;display.render();observe_fetch=false;
        assert(have_ball&&dog_lines.size()>=4);
        const auto pose=StickScenes::Detail::dogFetch(elapsed);
        const int dog_center=frame.x+frame.facing*static_cast<int>(std::lround(pose.dogX*358.0/256));
        const int ball_x=(ball_bounds.x1+ball_bounds.x2)/2,ball_y=(ball_bounds.y1+ball_bounds.y2)/2;
        const int expected_x=frame.x+frame.facing*static_cast<int>(std::lround(pose.ball.x*358.0/256));
        const int expected_y=frame.y+static_cast<int>(std::lround(pose.ball.y*358.0/256));
        assert(std::abs(ball_x-expected_x)<=1&&std::abs(ball_y-expected_y)<=1);
        unsigned visible_ball=0;
        for(int y=ball_bounds.y1;y<=ball_bounds.y2;++y)for(int x=ball_bounds.x1;x<=ball_bounds.x2;++x) {
            assert(x>=0&&x<Width&&y>=0&&y<Height);
            lv_color32_t rgb;rgb.full=lv_color_to32(display.pixels[y*Width+x]);
            visible_ball+=rgb.ch.red>rgb.ch.green+24&&rgb.ch.red>rgb.ch.blue+24;
        }
        assert(visible_ball>0);
        if(pose.carrying) {
            assert(pose.phase==StickScenes::Detail::DogFetchPhase::Turn||pose.phase==StickScenes::Detail::DogFetchPhase::Return||pose.phase==StickScenes::Detail::DogFetchPhase::Handoff);
            int nearest=10000;
            for(auto line:dog_lines)for(auto point:{line.from,line.to})
                nearest=std::min(nearest,std::max(std::abs(point.x-ball_x),std::abs(point.y-ball_y)));
            assert(nearest<=6); // The visible ball stays beside the projected muzzle.
        }
        std::vector<int> paws;
        for(unsigned leg=0;leg<4;++leg) {
            paws.push_back(dog_lines[leg].to.x-dog_center);paws.push_back(dog_lines[leg].to.y-frame.y);
        }
        for(int y=0;y<Height;++y)for(int x=0;x<Width;++x)if(display.pixels[y*Width+x].full!=lv_color_black().full) {
            assert(y>=frame.y-55&&y<=frame.y+7); // Entire native scene fits the lower clear stage.
            assert(frame.facing*(x-frame.x)>=-20&&frame.facing*(x-frame.x)<=215);
        }
        ++checks;return paws;
    }
};
static void checkFetchSequence() {
    using Phase=StickScenes::Detail::DogFetchPhase;
    const uint32_t moments[]={250,1000,1850,2400,2900,3800,4900,5500};
    const Phase phases[]={Phase::Windup,Phase::Throw,Phase::Chase,Phase::Pickup,Phase::Turn,Phase::Return,Phase::Handoff,Phase::Reset};
    const char *names[]={"windup","throw","chase","pickup","turn","return","handoff","reset"};
    for(int facing:{-1,1}) {
        FetchCanvas canvas(facing);std::set<uint64_t> images;
        for(unsigned phase=0;phase<8;++phase) {
            assert(StickScenes::Detail::dogFetch(moments[phase]).phase==phases[phase]);
            canvas.at(moments[phase]);images.insert(frameHash(canvas.display));
            if(facing==1)canvas.display.save(std::string("phase_")+names[phase]);
        }
        assert(images.size()>=7);
        int previous_dog=StickScenes::Detail::dogFetch(1600).dogX;
        std::set<std::vector<int>> running_paws,idle_paws;
        for(uint32_t elapsed=1600;elapsed<2200;elapsed+=32) {
            const auto pose=StickScenes::Detail::dogFetch(elapsed);assert(pose.dogX>=previous_dog);previous_dog=pose.dogX;
            running_paws.insert(canvas.at(elapsed));assert(pose.ball.x>=pose.dogX&&pose.ball.y==-2);
        }
        assert(running_paws.size()>=3);
        for(uint32_t elapsed=0;elapsed<400;elapsed+=32)idle_paws.insert(canvas.at(elapsed));
        assert(idle_paws.size()==1);
        previous_dog=StickScenes::Detail::dogFetch(3100).dogX;
        for(uint32_t elapsed=3100;elapsed<4700;elapsed+=64) {
            const auto pose=StickScenes::Detail::dogFetch(elapsed);assert(pose.dogX<=previous_dog);previous_dog=pose.dogX;
            assert(pose.carrying&&pose.facingQ8<0);canvas.at(elapsed);
        }
        // Touch the landed ball before carrying, then hand it back before resetting.
        const auto pickup=StickScenes::Detail::dogFetch(2700),returned=StickScenes::Detail::dogFetch(4700);
        assert(pickup.carrying&&pickup.ball.x==140&&pickup.ball.y==-2);
        assert(returned.ball.x==22&&returned.ball.y==-12);
        const auto handoff=StickScenes::Detail::dogFetch(5200),initial=StickScenes::Detail::dogFetch(0);
        assert(handoff.ball.x==initial.ball.x&&handoff.ball.y==initial.ball.y);
        for(uint32_t boundary:{500U,1600U,2200U,2700U,3100U,4700U,5200U,6000U,12000U}) {
            canvas.at(boundary-1);const auto before=dog_lines;const lv_area_t first_ball=ball_bounds;
            canvas.at(boundary+1);const auto after=dog_lines;
            assert(before.size()==after.size());
            assert(std::abs(ball_bounds.x1-first_ball.x1)<=3&&std::abs(ball_bounds.y1-first_ball.y1)<=3);
            for(size_t segment=0;segment<before.size();++segment) {
                assert(std::abs(before[segment].from.x-after[segment].from.x)<=3&&std::abs(before[segment].from.y-after[segment].from.y)<=3);
                assert(std::abs(before[segment].to.x-after[segment].to.x)<=3&&std::abs(before[segment].to.y-after[segment].to.y)<=3);
            }
        }
        const auto initial_paws=canvas.at(0);const lv_area_t initial_ball=ball_bounds;
        assert(canvas.at(6000)==initial_paws);
        assert(initial_ball.x1==ball_bounds.x1&&initial_ball.y1==ball_bounds.y1);
        const auto loop=StickScenes::Detail::dogFetch(6000);
        assert(loop.ball.x==initial.ball.x&&loop.ball.y==initial.ball.y&&loop.dogX==initial.dogX&&loop.facingQ8==initial.facingQ8);
    }
}

struct Routine {
    Display display;OdometerDisplay price;StickFigure figure;uint32_t now=0;
    explicit Routine(int facing=1) {
        price.begin(display.root(),&bodoni_72_smallcaps);price.setValue("$4,123.75");
        figure.begin(display.root(),1.4f);figure.setAutoplay(false);figure.setRandomSeed(37);figure.facing=facing;
        tick();assert(figure.playAnimation(ANIMATION_DOG));
    }
    ~Routine(){price.setAnimationEnabled(false);figure.setEnabled(false);lv_obj_clean(display.root());}
    void tick(uint32_t delta=16) {
        const int32_t old_x=figure.x,old_y=figure.y;now+=delta;lv_tick_inc(delta);lv_timer_handler();
        lv_area_t number,digit,reels[StickScenes::MaxReels];int direction=0;
        const bool has_number=price.numberBounds(&number),has_digit=price.interactionTarget(&digit,&direction);
        const size_t count=price.digitBounds(reels,StickScenes::MaxReels);
        figure.update(now,has_number?&number:nullptr,has_digit?&digit:nullptr,direction,price.isAnimating(),price.valueChangeDirection(),reels,count);
        lv_obj_update_layout(display.root());
        assert(std::abs(figure.x-old_x)<=figure.scaled(8)*256);
        if(old_y)assert(std::abs(figure.y-old_y)<=figure.scaled(8)*256);
        lv_area_t bounds;assert(figure.bounds(&bounds));assert(bounds.x1>=0&&bounds.y1>=0&&bounds.x2<Width&&bounds.y2<Height);
        ++checks;
    }
    void awaitPlay() {
        for(unsigned wait=0;figure.scenePhase!=StickFigure::ScenePhase::Play;wait+=16){assert(wait<8000);tick();}
        assert(figure.animationId()==ANIMATION_DOG);
    }
    void renderProtected(const std::string &name="") {
        lv_obj_add_flag(figure.actor,LV_OBJ_FLAG_HIDDEN);display.render();const auto expected=display.pixels;
        lv_obj_clear_flag(figure.actor,LV_OBJ_FLAG_HIDDEN);display.render();
        lv_area_t number;assert(price.numberBounds(&number));
        // Price ink covers the game; empty glyph cells and counters stay open.
        for(int y=number.y1;y<=number.y2;++y)for(int x=number.x1;x<=number.x2;++x)
            if(expected[y*Width+x].full==lv_color_white().full)
                assert(expected[y*Width+x].full==display.pixels[y*Width+x].full);
        if(!name.empty())display.save(name);
        ++checks;
    }
};
static void checkNativeRoutine() {
    for(int facing:{-1,1}) {
        Routine routine(facing);routine.awaitPlay();unsigned rendered=0;std::set<unsigned> phases;
        for(unsigned elapsed=0;routine.figure.isSceneActive();elapsed+=16) {
            assert(elapsed<17000);routine.tick();
            if(routine.figure.scenePhase==StickFigure::ScenePhase::Play) {
                const uint32_t age=routine.figure.poseTime-routine.figure.sceneStarted;
                phases.insert(static_cast<unsigned>(StickScenes::Detail::dogFetch(age).phase));
            }
            if(elapsed%96==0) {
                char name[64];std::snprintf(name,sizeof(name),"native_%04u",rendered);
                routine.renderProtected(facing==1?name:"");++rendered;
            }
        }
        assert(phases.size()==8&&rendered>80);
        std::printf("Native dog fetch facing %d: all eight phases, %u rendered price-protected frames.\n",facing,rendered);
    }
}
static void checkCancellation() {
    for(uint32_t age:{250U,1000U,1850U,2400U,2900U,3800U,4900U,5500U})for(bool disable:{false,true}) {
        Routine routine;routine.awaitPlay();while(routine.figure.poseTime-routine.figure.sceneStarted<age)routine.tick();
        const int32_t x=routine.figure.x,y=routine.figure.y;
        if(disable) {
            routine.figure.setEnabled(false);assert(!routine.figure.isSceneActive());
            routine.now+=4000;routine.figure.update(routine.now,nullptr,nullptr,0,false);
            assert(routine.figure.x==x&&routine.figure.y==y);routine.figure.setEnabled(true);
        } else {
            assert(routine.figure.playAnimation(ANIMATION_COINS));
            for(unsigned wait=0;routine.figure.animationId()!=ANIMATION_COINS;wait+=16) {
                assert(wait<1200);routine.tick();assert(routine.figure.x==x&&routine.figure.y==y);routine.renderProtected();
            }
        }
        assert(routine.figure.playAnimation(ANIMATION_DOG));
        for(unsigned wait=0;routine.figure.animationId()!=ANIMATION_DOG;wait+=16){assert(wait<1600);routine.tick();}
        routine.awaitPlay();routine.renderProtected();
    }
}
int main(int argc,char **argv) {
    preview_directory=argc>1?argv[1]:nullptr;lv_init();checkFetchSequence();checkNativeRoutine();checkCancellation();
    std::printf("PASS %u native dog-fetch throw/chase/pickup/carry/handoff, gait, continuity, price protection and lifecycle checks.\n",checks);
}
