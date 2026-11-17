
#include "libax.h"
#include "axipc.h"
#include "unistd.h"

#define Width 400
#define Height 260
#define ONE 1024
#define STEP 18
#define TURN 18
#define FOV 680
#define MAP_N 8

#define CEILING AX_RGB(35,35,42)
#define FLOOR AX_RGB(25,90,205)
#define WALL_X AX_RGB(255,125,25)
#define WALL_Y AX_RGB(190,75,20)

static const char *world[MAP_N]={
    "11111111","10000001","10110001","10000001",
    "10001101","10000001","10000001","11111111"
};

static int px=ONE*3/2,py=ONE*3/2,dx=ONE,dy;
static int held[6],frames,fps,last_sec=-1;

enum {FWD,BACK,LEFT,RIGHT,TURN_L,TURN_R};

static const struct {uint8_t scan,ext;} binds[6]={
    {0x11,0},{0x1F,0},{0x1E,0},
    {0x20,0},{0x4B,1},{0x4D,1}
};

static int abs_i(int v){return v<0?-v:v;}

static int is_wall(int x,int y){
    return x<0||y<0||x>=MAP_N||y>=MAP_N||world[y][x]=='1';
}

static int update(void){
    int f=held[FWD]-held[BACK];
    int s=held[RIGHT]-held[LEFT];
    int t=held[TURN_R]-held[TURN_L];

    if(!f&&!s&&!t)return 0;

    int mx=(dx*f-dy*s)*STEP/ONE;
    int my=(dy*f+dx*s)*STEP/ONE;

    if(!is_wall((px+mx*6)/ONE,py/ONE))px+=mx;
    if(!is_wall(px/ONE,(py+my*6)/ONE))py+=my;

    if(t){
        dx-=dy*t*TURN/ONE;
        dy+=dx*t*TURN/ONE;
    }
    return 1;
}

static void axis_init(int r,int pos,int cell,int *step,int *delta,int *side){
    int a=abs_i(r);
    if(!a)a=1;
    *step=r<0?-1:1;
    *delta=ONE*ONE/a;
    *side=(r<0?pos-cell*ONE:(cell+1)*ONE-pos)*ONE/a;
}

static void draw_column(int x,int planeX,int planeY){
    int cam=2*x*ONE/g_cw-ONE;
    int rx=dx+planeX*cam/ONE,ry=dy+planeY*cam/ONE;
    int mx=px/ONE,my=py/ONE;
    int sx,sy,dx1,dy1,sideX,sideY;

    axis_init(rx,px,mx,&sx,&dx1,&sideX);
    axis_init(ry,py,my,&sy,&dy1,&sideY);

    int side=0;
    for(int i=0;i<32;i++){
        if(sideX<sideY)sideX+=dx1,mx+=sx,side=0;
        else sideY+=dy1,my+=sy,side=1;
        if(is_wall(mx,my))break;
    }

    int dist=side?sideY-dy1:sideX-dx1;
    if(dist<1)dist=1;

    int h=g_ch*ONE/dist;
    if(h>g_ch)h=g_ch;
    ax_rect(x,(g_ch-h)/2,1,h,side?WALL_Y:WALL_X);
}

static void draw_fps(void){
    char text[16],tmp[8];
    int v=fps,i=5,k=0;

    text[0]='F';text[1]='P';text[2]='S';
    text[3]=':';text[4]=' ';

    if(v>9999)v=9999;

    if(!v)text[i++]='0';
    else{
        while(v&&k<7){tmp[k++]='0'+v%10;v/=10;}
        while(k)text[i++]=tmp[--k];
    }

    text[i]=0;
    ax_text(text,8,8,AX_RGB(120,220,140));
}

static void count_fps(void){
    ax_time_t t;
    ax_time(&t);
    frames++;
    if(t.second!=last_sec){
        last_sec=t.second;
        fps=frames;
        frames=0;
    }
}

static void render(void){
    int planeX=-dy*FOV/ONE;
    int planeY=dx*FOV/ONE;

    ax_fill(CEILING);
    ax_rect(0,g_ch/2,g_cw,g_ch/2,FLOOR);

    for(int x=0;x<g_cw;x++)
        draw_column(x,planeX,planeY);

    draw_fps();
}

static void on_rawkey(const ax_event *ev){
    for(int i=0;i<6;i++)
        if(binds[i].scan==ev->key&&binds[i].ext==(ev->buttons&1))
            held[i]=ev->w!=0;
}

void _start(void){
    uint32_t *canvas=ax_surface("app",Width,Height);
    ax_event ev;

    if(!canvas)return;

    for(;;){
        while(ax_poll(canvas,&ev)>0){
            if(ev.type==AX_EV_CLOSE||ev.type==AX_EV_QUIT){
                ax_win_close(canvas);
                return;
            }
            if(ev.type==AX_EV_RAWKEY)on_rawkey(&ev);
        }

        update();
        count_fps();
        render();
        ax_commit(canvas);
        ax_yield();
    }
}
