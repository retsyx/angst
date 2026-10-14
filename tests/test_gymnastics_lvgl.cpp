// Exercise the actual native geometry and LVGL rasterizer for a moving routine.
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
struct PaintedLine { lv_point_t from, to; };
static std::vector<PaintedLine> painted_body;
static bool observe_body;
static void observedLine(lv_draw_ctx_t *context, const lv_draw_line_dsc_t *style,
                         const lv_point_t *from, const lv_point_t *to) {
    if(observe_body && style->color.full==lv_color_hex(0xf2c87b).full && style->opa==LV_OPA_COVER && painted_body.size()<10)
        painted_body.push_back({*from,*to});
    lv_draw_line(context,style,from,to);
}
#include "odometer.h"
#define private public
#define lv_draw_line observedLine
#include "stick_figure.h"
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
    ~Display(){lv_disp_remove(display);}
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

struct PoseCanvas {
    Display display;lv_obj_t *canvas;StickScenes::Frame frame={};
    explicit PoseCanvas(int facing) {
        frame.id=ANIMATION_GYMNASTICS;frame.duration=animation_catalog_lookup(ANIMATION_GYMNASTICS)->duration_ms;
        frame.x=Width/2;frame.y=Height-15;frame.facing=facing;frame.scaleQ8=358;
        frame.width=Width;frame.height=Height;frame.opacity=LV_OPA_COVER;
        canvas=lv_obj_create(display.root());lv_obj_remove_style_all(canvas);lv_obj_clear_flag(canvas,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(canvas,Width,Height);
        lv_obj_add_event_cb(canvas,[](lv_event_t *event){auto *self=static_cast<PoseCanvas *>(lv_event_get_user_data(event));
            lv_area_t bounds;lv_obj_get_coords(lv_event_get_target(event),&bounds);
            StickScenes::draw(lv_event_get_draw_ctx(event),bounds,self->frame);
        },LV_EVENT_DRAW_MAIN,this);
    }
    std::vector<PaintedLine> at(uint32_t elapsed) {
        frame.elapsed=elapsed;painted_body.clear();observe_body=true;display.render();observe_body=false;
        assert(painted_body.size()==10);
        for(auto line:painted_body)for(auto point:{line.from,line.to}) {
            assert(std::abs(point.x-frame.x)<=33);assert(point.y>=frame.y-54&&point.y<=frame.y+5);
        }
        for(int y=0;y<Height;++y)for(int x=0;x<Width;++x)if(display.pixels[y*Width+x].full!=lv_color_black().full) {
            assert(std::abs(x-frame.x)<=34);assert(y>=frame.y-55&&y<=frame.y+6);
        }
        ++checks;return painted_body;
    }
};
static void checkManeuvers() {
    for(int facing:{-1,1}) {
        PoseCanvas canvas(facing);const auto neutral=canvas.at(0);
        const uint32_t samples[]={1500,3000,4500,6800};const char *names[]={"cartwheel","jump","somersault","handspring"};
        std::set<uint64_t> shapes;
        for(unsigned move=0;move<4;++move) {
            canvas.at(samples[move]);shapes.insert(frameHash(canvas.display));
            if(facing==1)canvas.display.save(std::string("pose_")+names[move]);
        }
        assert(shapes.size()==4);
        const auto cartwheel=canvas.at(1500);
        const auto jump=canvas.at(3000);
        const auto somersault=canvas.at(4500);
        const auto handspring=canvas.at(6800);
        // Actual rendered spine/limbs must show inversion and an airborne tuck.
        assert(cartwheel[5].to.y>cartwheel[4].from.y);
        assert(cartwheel[1].to.y<cartwheel[4].from.y||cartwheel[3].to.y<cartwheel[4].from.y);
        assert(jump[4].from.y<neutral[4].from.y-8);
        assert(somersault[5].to.y>somersault[4].from.y);
        assert(handspring[7].to.y>handspring[4].from.y||handspring[9].to.y>handspring[4].from.y);
        for(uint32_t boundary:{600U,2400U,3600U,5400U,5800U,7800U,9600U,11000U}) {
            const auto before=canvas.at(boundary-1),at=canvas.at(boundary),after=canvas.at(boundary+1);
            for(size_t segment=0;segment<at.size();++segment) {
                // Every maneuver joins the same neutral articulated skeleton.
                assert(at[segment].from.x==neutral[segment].from.x&&at[segment].from.y==neutral[segment].from.y);
                assert(at[segment].to.x==neutral[segment].to.x&&at[segment].to.y==neutral[segment].to.y);
                for(const auto &pair:{std::make_pair(before[segment],at[segment]),std::make_pair(at[segment],after[segment])}) {
                    assert(std::abs(pair.first.from.x-pair.second.from.x)<=2&&std::abs(pair.first.from.y-pair.second.from.y)<=2);
                    assert(std::abs(pair.first.to.x-pair.second.to.x)<=2&&std::abs(pair.first.to.y-pair.second.to.y)<=2);
                }
            }
        }
    }
}

struct Routine {
    Display display;OdometerDisplay price;StickFigure figure;uint32_t now=0;
    explicit Routine(int facing=1) {
        price.begin(display.root(),&bodoni_72_smallcaps);price.setValue("$4,123.75");
        figure.begin(display.root(),1.4f);figure.setAutoplay(false);figure.setRandomSeed(37);figure.facing=facing;
        tick();assert(figure.playAnimation(ANIMATION_GYMNASTICS));
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
        assert(figure.animationId()==ANIMATION_GYMNASTICS);
    }
    void renderProtected(const std::string &name="") {
        lv_obj_add_flag(figure.actor,LV_OBJ_FLAG_HIDDEN);display.render();const auto expected=display.pixels;
        lv_obj_clear_flag(figure.actor,LV_OBJ_FLAG_HIDDEN);display.render();
        lv_area_t number;assert(price.numberBounds(&number));
        // Foreground glyph strokes remain opaque; their counters and margins
        // are transparent to the gymnastics poses underneath.
        for(int y=number.y1;y<=number.y2;++y)for(int x=number.x1;x<=number.x2;++x)
            if(expected[y*Width+x].full==lv_color_white().full)
                assert(expected[y*Width+x].full==display.pixels[y*Width+x].full);
        if(!name.empty())display.save(name);
        ++checks;
    }
};
static void checkTraversal() {
    for(int initial_facing:{-1,1}) {
        Routine routine(initial_facing);routine.awaitPlay();
        int minimum=Width,maximum=0;bool backward_handspring=false;std::set<int> facings;
        unsigned renders=0;bool saw_exit=false;
        for(unsigned elapsed=0;routine.figure.isSceneActive();elapsed+=16) {
            assert(elapsed<14000);const int32_t previous_x=routine.figure.x;
            const int previous_facing=routine.figure.facing;
            const unsigned previous_segment=StickScenes::Detail::gymnasticsMotion(routine.figure.poseTime-routine.figure.sceneStarted,11000).segment;
            routine.tick();minimum=std::min(minimum,static_cast<int>(routine.figure.x/256));maximum=std::max(maximum,static_cast<int>(routine.figure.x/256));
            if(routine.figure.scenePhase==StickFigure::ScenePhase::Play) {
                const auto move=StickScenes::Detail::gymnasticsMotion(routine.figure.poseTime-routine.figure.sceneStarted,11000);
                if(move.segment==previous_segment)assert(routine.figure.facing==previous_facing);
                if(move.action==StickScenes::Detail::GymnasticsAction::Handspring&&routine.figure.x!=previous_x)
                    backward_handspring|=(routine.figure.x-previous_x)*routine.figure.facing<0;
                facings.insert(routine.figure.facing);
            }
            saw_exit|=routine.figure.scenePhase==StickFigure::ScenePhase::Exit;
            if(elapsed%128==0) {
                char name[64];std::snprintf(name,sizeof(name),"native_%04u",renders);
                routine.renderProtected(initial_facing==1?name:"");++renders;
            }
        }
        assert(maximum-minimum>Width/2);assert(backward_handspring&&facings.size()==2&&saw_exit&&renders>40);
        const int32_t finished_x=routine.figure.x,finished_y=routine.figure.y;
        routine.figure.setAutoplay(true);routine.tick();assert(!routine.figure.isSceneActive());
        assert(std::abs(routine.figure.x-finished_x)<=routine.figure.scaled(8)*256&&std::abs(routine.figure.y-finished_y)<=routine.figure.scaled(8)*256);
        for(unsigned elapsed=0;elapsed<1024;elapsed+=16)routine.tick();
        assert(routine.figure.x!=finished_x||routine.figure.y!=finished_y);
        std::printf("Native gymnastics facing %d: %d pixels of actor travel, %u rendered protected frames.\n",initial_facing,maximum-minimum,renders);
    }
}
static void checkCancellation() {
    for(uint32_t age:{1500U,3000U,4500U,6800U})for(bool disable:{false,true}) {
        Routine routine;routine.awaitPlay();
        while(routine.figure.poseTime-routine.figure.sceneStarted<age)routine.tick();
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
            assert(routine.figure.scenePhase==StickFigure::ScenePhase::Enter);
        }
        assert(routine.figure.playAnimation(ANIMATION_GYMNASTICS));
        for(unsigned wait=0;routine.figure.animationId()!=ANIMATION_GYMNASTICS;wait+=16){assert(wait<1600);routine.tick();}
        routine.awaitPlay();routine.renderProtected();
    }
}
int main(int argc,char **argv) {
    preview_directory=argc>1?argv[1]:nullptr;lv_init();checkManeuvers();checkTraversal();checkCancellation();
    std::printf("PASS %u native gymnastics pose, travel, continuity, lifecycle, price-protection and cancellation checks.\n",checks);
}
