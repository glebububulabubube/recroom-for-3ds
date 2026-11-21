#include <3ds.h>
#include <citro2d.h>
#include <math.h>
#include <stdbool.h>

#define SCR_WIDTH  320
#define SCR_HEIGHT 240
#define FOV 1.0f

typedef struct { float x, y, z; } Vec3;

static float clampf(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v); }
static void vsub(Vec3* o, const Vec3* a, const Vec3* b){ o->x=a->x-b->x; o->y=a->y-b->y; o->z=a->z-b->z; }
static float vdot(const Vec3* a, const Vec3* b){ return a->x*b->x + a->y*b->y + a->z*b->z; }
static float vlen(const Vec3* a){ return sqrtf(vdot(a,a)); }

typedef struct { Vec3 p; Vec3 c; float size; bool heldL, heldR; } Box;

#define MAX_BOXES 32
#define ROOM_MIN_Z -6.0f
#define ROOM_MAX_Z  6.0f
#define ROOM_MIN_X -6.0f
#define ROOM_MAX_X  6.0f
#define FLOOR_Y   -1.0f
#define CEIL_Y     3.0f

static Box boxes[MAX_BOXES];
static int boxCount = 0;

static float pyaw=0, ppitch=0;
static Vec3 cam={0,0,0};
static Vec3 handL={ 0.7f, 0.6f,  1.0f};
static Vec3 handR={-0.7f, 0.6f,  1.0f};
static bool grabL=false, grabR=false;

static void addBox(float x,float y,float z,float s,float r,float g,float b){
    if(boxCount>=MAX_BOXES) return;
    Box* q=&boxes[boxCount++];
    q->p.x=x; q->p.y=y; q->p.z=z;
    q->c.x=r; q->c.y=g; q->c.z=b;
    q->size=s; q->heldL=q->heldR=false;
}

static void initWorld(void){
    addBox(-2, 0.2f,  2, 1.4f, 0.85f, 0.3f, 0.3f);
    addBox( 3, 0.2f, -1, 1.2f, 0.3f, 0.85f, 0.3f);
    addBox( 0, 0.5f, -3, 1.6f, 0.35f, 0.4f, 0.9f);
    addBox( 1, 0.4f,  0, 0.9f, 1.0f, 0.85f, 0.1f);
    addBox(-3, 0.6f, -1, 1.0f, 0.9f, 0.35f, 0.35f);
    addBox( 0, 0.2f,  0, 0.75f, 0.6f, 0.6f, 0.6f);
}

// world -> screen. returns false if behind camera.
static bool project(const Vec3* w, float* sx, float* sy, float* depth){
    Vec3 d; vsub(&d, w, &cam);
    float cy=cosf(-pyaw), syy=sinf(-pyaw);
    float x=d.x*cy + d.z*syy;
    float z=-d.x*syy + d.z*cy;
    float cp=cosf(-ppitch), sp=sinf(-ppitch);
    float y=d.y*cp - z*sp;
    z= d.y*sp + z*cp;
    if(z<=0.05f) return false;
    float s=(float)SCR_HEIGHT/(FOV*z);
    *sx=SCR_WIDTH/2.0f + x*s;
    *sy=SCR_HEIGHT/2.0f - y*s;
    *depth=z;
    return true;
}

static void drawSky(C2D_Target* top){
    C2D_DrawRectSolid(0,0,0,SCR_WIDTH,SCR_HEIGHT, C2D_Color32(90,150,220,255));
}

static void drawBox(C2D_Target* top, Box* b){
    Vec3 min={b->p.x-b->size/2,b->p.y-b->size/2,b->p.z-b->size/2};
    Vec3 max={b->p.x+b->size/2,b->p.y+b->size/2,b->p.z+b->size/2};
    float sx0,sy0,sz0,sx1,sy1,sz1;
    if(!project(&min,&sx0,&sy0,&sz0)) return;
    if(!project(&max,&sx1,&sy1,&sz1)) return;
    float x0=fminf(sx0,sx1), x1=fmaxf(sx0,sx1);
    float y0=fminf(sy0,sy1), y1=fmaxf(sy0,sy1);
    if(x0<0)x0=0; if(x1>SCR_WIDTH)x1=SCR_WIDTH;
    if(y0<0)y0=0; if(y1>SCR_HEIGHT)y1=SCR_HEIGHT;
    if(x1<=x0||y1<=y0) return;
    u32 col=C2D_Color32((u8)(b->c.x*255),(u8)(b->c.y*255),(u8)(b->c.z*255),255);
    u32 dark=C2D_Color32((u8)(b->c.x*150),(u8)(b->c.y*150),(u8)(b->c.z*150),255);
    C2D_DrawRectSolid(x0,y0,0.5f,x1-x0,y1-y0,col);
    float h=(y1-y0)*0.22f;
    C2D_DrawRectSolid(x0,y1-h,0.51f,x1-x0,h,dark);
}

static void drawHand(C2D_Target* top, const Vec3* h, bool active){
    Vec3 w={cam.x+h->x, cam.y+h->y, cam.z+h->z};
    float sx,sy,dz; if(!project(&w,&sx,&sy,&dz)) return;
    float r=10.0f/(dz*0.18f); r=clampf(r,5,16);
    u32 col= active ? C2D_Color32(60,255,90,255) : C2D_Color32(255,255,255,220);
    C2D_DrawCircleSolid(sx,sy,0.6f,r,col);
}

static Box* nearestTo(const Vec3* handWorld, float maxDist){
    Box* best=NULL; float bestD=maxDist;
    for(int i=0;i<boxCount;i++){
        Box* b=&boxes[i];
        if(b->heldL||b->heldR) continue;
        Vec3 to; vsub(&to,handWorld,&b->p);
        float d=vlen(&to);
        if(d<bestD){bestD=d; best=b;}
    }
    return best;
}

static void doGrab(C2D_Target* bottom, bool left, bool* grabFlag, const Vec3* handRel){
    Vec3 hw={cam.x+handRel->x, cam.y+handRel->y, cam.z+handRel->z};
    if(*grabFlag){
        *grabFlag=false;
        for(int i=0;i<boxCount;i++) boxes[i].heldL=boxes[i].heldR=false; // sanitize
    } else {
        Box* b=nearestTo(&hw,1.4f);
        if(b){ if(left)b->heldL=true; else b->heldR=true; *grabFlag=true; }
    }
}

int main(void){
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();
    C2D_Target* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C2D_Target* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    initWorld();

    while(aptMainLoop()){
        hidScanInput();
        u32 kDown=hidKeysDown();
        u32 kHeld=hidKeysHeld();
        circlePosition cp; hidCircleRead(&cp);

        if(kDown & KEY_START) break;

        bool lHandMode=(kHeld & KEY_L);
        bool rHandMode=(kHeld & KEY_R);

        // vertical hand offsets held on dpad (used in hand mode)
        float vAxisL = (kHeld&KEY_DUP?1:(kHeld&KEY_DDOWN?-1:0));
        float vAxisR = vAxisL; // shared; in practice chosen by hand mode

        if(!lHandMode && !rHandMode){
            // move/look when no hand mode
            float fx=-sinf(pyaw), fz=-cosf(pyaw);
            float rx=cosf(pyaw), rz=-sinf(pyaw);
            float fwd=(float)cp.dy/90.0f;
            float strafe=(float)cp.dx/90.0f;
            cam.x+=(fx*fwd + rx*strafe)*-0.06f;
            cam.z+=(fz*fwd + rz*strafe)*-0.06f;
            cam.x=clampf(cam.x,ROOM_MIN_X+1,ROOM_MAX_X-1);
            cam.z=clampf(cam.z,ROOM_MIN_Z+1,ROOM_MAX_Z-1);
            cam.y=0.0f;
            pyaw+=(kHeld&KEY_DLEFT?0.05f:(kHeld&KEY_DRIGHT?-0.05f:0));
            ppitch+=(kHeld&KEY_DUP?0.04f:(kHeld&KEY_DDOWN?-0.04f:0));
            ppitch=clampf(ppitch,-1.2f,1.2f);
        }

        if(lHandMode){
            float dx=-(float)cp.dx/90.0f, dy=-(float)cp.dy/90.0f;
            float fx=-sinf(pyaw), fz=-cosf(pyaw);
            float rx2=cosf(pyaw), rz2=-sinf(pyaw);
            handL.x+=(rx2*dx + fx*dy)*0.07f;
            handL.z+=(rz2*dx + fz*dy)*0.07f;
            handL.y+=vAxisL*0.05f;
            handL.x=clampf(handL.x,-1.5f,1.5f);
            handL.y=clampf(handL.y,-1.2f,1.2f);
            handL.z=clampf(handL.z,0.3f,3.0f);
            if(kDown&KEY_X) doGrab(bottom,true,&grabL,&handL);
        }
        if(rHandMode){
            float dx=-(float)cp.dx/90.0f, dy=-(float)cp.dy/90.0f;
            float fx=-sinf(pyaw), fz=-cosf(pyaw);
            float rx2=cosf(pyaw), rz2=-sinf(pyaw);
            handR.x+=(rx2*dx + fx*dy)*0.07f;
            handR.z+=(rz2*dx + fz*dy)*0.07f;
            handR.y+=vAxisR*0.05f;
            handR.x=clampf(handR.x,-1.5f,1.5f);
            handR.y=clampf(handR.y,-1.2f,1.2f);
            handR.z=clampf(handR.z,0.3f,3.0f);
            if(kDown&KEY_X) doGrab(bottom,false,&grabR,&handR);
        }

        // attach held boxes to hands
        for(int i=0;i<boxCount;i++){
            Box* b=&boxes[i];
            if(b->heldL){ b->p.x=cam.x+handL.x; b->p.y=cam.y+handL.y-0.25f; b->p.z=cam.z+handL.z; }
            else if(b->heldR){ b->p.x=cam.x+handR.x; b->p.y=cam.y+handR.y-0.25f; b->p.z=cam.z+handR.z; }
            b->p.y=clampf(b->p.y,FLOOR_Y+0.2f,CEIL_Y-0.2f);
        }

        // render top (first person)
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_SceneBegin(top);
        drawSky(top);
        // floor grid
        for(int gx=-5;gx<=5;gx++)for(int gz=-5;gz<=5;gz++){
            Vec3 p={(float)gx, FLOOR_Y, (float)gz};
            float sx,sy,d; if(project(&p,&sx,&sy,&d)){
                if(d>3) continue;
                float a=clampf(1.0f-(d-3.0f)/14.0f,0.15f,0.7f);
                C2D_DrawRectSolid(sx-1,sy-1,0.1f,2,2,C2D_Color32((u8)(110*a),(u8)(110*a),(u8)(120*a),255));
            }
        }
        // boxes far->near
        {
            static int order[MAX_BOXES]; static float dv[MAX_BOXES];
            for(int i=0;i<boxCount;i++){ order[i]=i; Vec3 d; vsub(&d,&cam,&boxes[i].p); dv[i]=vdot(&d,&d);}
            for(int i=1;i<boxCount;i++){int j=i;while(j>0&&dv[order[j]]>dv[order[j-1]]){int t=order[j];order[j]=order[j-1];order[j-1]=t;j--;}}
            for(int i=0;i<boxCount;i++) drawBox(top,&boxes[order[i]]);
        }
        drawHand(top,&handL,grabL);
        drawHand(top,&handR,grabR);
        C2D_Flush();

        // render bottom (status)
        C2D_SceneBegin(bottom);
        C2D_DrawRectSolid(40,100,0.5f,40,30, lHandMode?C2D_Color32(60,255,90,255):C2D_Color32(70,70,70,255));
        C2D_DrawRectSolid(240,100,0.5f,40,30, rHandMode?C2D_Color32(255,150,40,255):C2D_Color32(70,70,70,255));
        C2D_DrawRectSolid(40,160,0.5f,240,16, C2D_Color32(40,40,50,255));
        if(grabL) C2D_DrawCircleSolid(60,168,0.51f,6, C2D_Color32(0,255,0,255));
        if(grabR) C2D_DrawCircleSolid(240,168,0.51f,6, C2D_Color32(255,150,40,255));
        C2D_Flush();
        C3D_FrameEnd(0);
    }

    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}