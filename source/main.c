#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <string.h>
#include <stdbool.h>
#include "vshader_shbin.h"

#define TOP_W 400
#define TOP_H 240

#define DISPLAY_TRANSFER_FLAGS \
	(GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
	GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
	GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define CLEAR_COLOR 0x5A96DCFF

typedef struct { float x, y, z; } V3;
typedef struct { float r, g, b; } V3c;

// ---- world ----
typedef struct { V3 pos; V3c col; float size; bool heldL, heldR; } Box;
#define MAX_BOXES 64
static Box boxes[MAX_BOXES];
static int boxCount = 0;

// ---- camera / hands ----
static V3 cam = {0, 0, 0};
static float yaw = 0.0f, pitch = 0.0f;
static V3 handL = {-0.35f, -0.28f, 0.55f};
static V3 handR = { 0.35f, -0.28f, 0.55f};
static int heldLIdx = -1, heldRIdx = -1;

// ---- shader ----
static DVLB_s* dvlb;
static shaderProgram_s program;
static int uLoc_modelView, uLoc_projection;
static C3D_Mtx projection;
static C3D_RenderTarget* top;
static C3D_RenderTarget* bottom;

// unit cube (half-extent 1, centered at origin): 8 corners, 36 indices
static const float CUBE_V[8][3] = {
    {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
    {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1},
};
static const int CUBE_T[36] = {
    0,1,2, 0,2,3,   // back (z=-)
    5,4,7, 5,7,6,   // front (z=+)
    4,0,3, 4,3,7,   // left
    1,5,6, 1,6,2,   // right
    3,2,6, 3,6,7,   // top
    4,5,1, 4,1,0,   // bottom
};
static const float FACE_SHADE[6] = {0.7f, 0.9f, 0.6f, 0.8f, 1.0f, 0.5f};

static float clampf(float v, float lo, float hi){ return v<lo?lo:(v>hi?hi:v); }

static void addBox(float x,float y,float z,float s,float r,float g,float b){
    if(boxCount>=MAX_BOXES) return;
    Box* q=&boxes[boxCount++];
    q->pos.x=x; q->pos.y=y; q->pos.z=z;
    q->col.r=r; q->col.g=g; q->col.b=b;
    q->size=s; q->heldL=q->heldR=false;
}

static void initWorld(void){
    addBox(0, -1.05f, 0, 14.0f, 0.45f, 0.45f, 0.5f); // floor slab
    addBox(-2, 0.25f,  2, 1.4f, 0.85f, 0.30f, 0.30f);
    addBox( 3, 0.22f, -1, 1.2f, 0.30f, 0.85f, 0.30f);
    addBox( 0, 0.50f, -3, 1.6f, 0.35f, 0.40f, 0.90f);
    addBox( 1, 0.40f,  0, 0.9f, 1.00f, 0.85f, 0.10f);
    addBox(-3, 0.60f, -1, 1.0f, 0.90f, 0.35f, 0.35f);
    addBox( 0, 0.25f,  0, 0.75f, 0.60f, 0.60f, 0.60f);
}

// ---- init shader (same layout as official simple_tri example) ----
static void sceneInit(void){
    dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
    C3D_BindProgram(&program);

    uLoc_modelView  = shaderInstanceGetUniformLocation(program.vertexShader, "modelView");
    uLoc_projection = shaderInstanceGetUniformLocation(program.vertexShader, "projection");

    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4); // v0 = position xyzw
    AttrInfo_AddFixed(ai, 1);                // v1 = color (fixed attribute)

    C3D_TexEnv* env = C3D_GetTexEnv(0);
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);

    Mtx_PerspStereoTilt(&projection, 60.0f*M_PI/180.0f, (float)TOP_W/TOP_H, 0.05f, 100.0f,
                        0.0f, 0.0f, 0.0f);
}

static void fillBoth(u32 color){
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(top, C3D_CLEAR_ALL, color, 0);
    C3D_FrameDrawOn(top);
    C3D_RenderTargetClear(bottom, C3D_CLEAR_ALL, color, 0);
    C3D_FrameDrawOn(bottom);
    C3D_FrameEnd(0);
}

static void buildView(C3D_Mtx* out){
    C3D_Mtx tmp, ry, rx;
    Mtx_Identity(&tmp);
    Mtx_Translate(&tmp, -cam.x, -cam.y, -cam.z, true);

    Mtx_Identity(&ry);
    Mtx_RotateY(&ry, -yaw, true);

    Mtx_Identity(&rx);
    Mtx_RotateX(&rx, -pitch, true);

    C3D_Mtx a; Mtx_Multiply(&a, &ry, &tmp);
    Mtx_Multiply(out, &rx, &a);
}

// draw one cube: 6 faces drawn separately, each with its own fixed color (light shading)
static void drawCubeAt(C3D_Mtx* view, V3 pos, float size, float r, float g, float b){
    C3D_Mtx model, scale, tmpmv, mv;
    Mtx_Identity(&model);
    model.m[12]=pos.x; model.m[13]=pos.y; model.m[14]=pos.z;

    Mtx_Identity(&scale);
    scale.m[0]=scale.m[5]=scale.m[10]=size;

    Mtx_Multiply(&tmpmv, &scale, &model);
    Mtx_Multiply(&mv, view, &tmpmv);

    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_modelView, &mv);

    for(int f=0; f<6; f++){
        float sh=FACE_SHADE[f];
        C3D_FixedAttribSet(1, r*sh, g*sh, b*sh, 1.0f);

        C3D_ImmDrawBegin(GPU_TRIANGLES);
        for(int t=0; t<6; t++){
            int vi=CUBE_T[f*6+t];
            C3D_ImmSendAttrib(CUBE_V[vi][0], CUBE_V[vi][1], CUBE_V[vi][2], 1.0f);
        }
        C3D_ImmDrawEnd();
    }
}

// world position from a camera-relative offset
static void toWorld(V3* w, const V3* rel){
    float cy=cosf(yaw), sy=sinf(yaw), cp=cosf(pitch), sp=sinf(pitch);
    V3 f={-sy*cp, sp, -cy*cp};
    V3 r_={cy, 0.0f, -sy};
    V3 u={-sp*cy, cp, sp*sy};
    w->x = cam.x + f.x*rel->z + r_.x*rel->x + u.x*rel->y;
    w->y = cam.y + f.y*rel->z + r_.y*rel->x + u.y*rel->y;
    w->z = cam.z + f.z*rel->z + r_.z*rel->x + u.z*rel->y;
}

static int nearestTo(const V3* w, int except){
    int best=-1; float bestD=1.4f*1.4f;
    for(int i=0;i<boxCount;i++){
        if(i==except) continue;
        Box* b=&boxes[i];
        if(b->heldL||b->heldR) continue;
        if(b->size>=10.0f) continue; // don't grab the floor
        float dx=w->x-b->pos.x, dy=w->y-b->pos.y, dz=w->z-b->pos.z;
        float d=dx*dx+dy*dy+dz*dz;
        if(d<bestD && d>0.0f){bestD=d; best=i;}
    }
    return best;
}

static void doGrabLeft(void){
    if(heldLIdx>=0){ boxes[heldLIdx].heldL=false; heldLIdx=-1; }
    else {
        V3 w; toWorld(&w, &handL);
        int i=nearestTo(&w, heldRIdx);
        if(i>=0){ boxes[i].heldL=true; heldLIdx=i; }
    }
}
static void doGrabRight(void){
    if(heldRIdx>=0){ boxes[heldRIdx].heldR=false; heldRIdx=-1; }
    else {
        V3 w; toWorld(&w, &handR);
        int i=nearestTo(&w, heldLIdx);
        if(i>=0){ boxes[i].heldR=true; heldRIdx=i; }
    }
}

int main(void){
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

    top = C3D_RenderTargetCreate(TOP_W, TOP_H, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    bottom = C3D_RenderTargetCreate(320, 240, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(bottom, GFX_BOTTOM, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

    fillBoth(0xFF0000FF);  // КРАСНЫЙ: GPU и таргеты инициализированы

    sceneInit();

    fillBoth(0x00FF00FF);  // ЗЕЛЁНЫЙ: шейдер скомпилирован и загружен

    initWorld();

    fillBoth(0x0000FFFF);  // СИНИЙ: мир создан, входим в игровой цикл

    while(aptMainLoop()){
        hidScanInput();
        u32 kDown=hidKeysDown();
        u32 kHeld=hidKeysHeld();
        circlePosition cp; hidCircleRead(&cp);

        if(kDown & KEY_START) break;

        bool lHand=(kHeld & KEY_L), rHand=(kHeld & KEY_R);

        if(!lHand && !rHand){
            float cy=cosf(yaw), sy=sinf(yaw), cp2=cosf(pitch);
            float fwd= (float)cp.dy/90.0f, strafe=(float)cp.dx/90.0f;
            cam.x += (-sy*cp2)*fwd*0.06f + (cy)*strafe*0.06f;
            cam.z += (-cy*cp2)*fwd*0.06f + (-sy)*strafe*0.06f;
            cam.x=clampf(cam.x,-6,6); cam.z=clampf(cam.z,-6,6);
            cam.y=0.0f;
            yaw   += (kHeld&KEY_DLEFT?0.05f:(kHeld&KEY_DRIGHT?-0.05f:0));
            pitch += (kHeld&KEY_DUP?-0.04f:(kHeld&KEY_DDOWN?0.04f:0));
            pitch = clampf(pitch,-1.2f,1.2f);
        }

        float vAxis=(kHeld&KEY_DUP?1:(kHeld&KEY_DDOWN?-1:0));
        if(lHand){
            handL.x += (float)cp.dx/90.0f * 0.015f;
            handL.y += vAxis*0.02f;
            handL.z -= (float)cp.dy/90.0f * 0.015f;
            handL.x=clampf(handL.x,-0.9f,0.9f);
            handL.y=clampf(handL.y,-0.5f,0.5f);
            handL.z=clampf(handL.z,0.25f,0.9f);
            if(kDown&KEY_X) doGrabLeft();
        }
        if(rHand){
            handR.x += (float)cp.dx/90.0f * 0.015f;
            handR.y += vAxis*0.02f;
            handR.z -= (float)cp.dy/90.0f * 0.015f;
            handR.x=clampf(handR.x,-0.9f,0.9f);
            handR.y=clampf(handR.y,-0.5f,0.5f);
            handR.z=clampf(handR.z,0.25f,0.9f);
            if(kDown&KEY_X) doGrabRight();
        }

        if(heldLIdx>=0){
            V3 w; toWorld(&w,&handL); boxes[heldLIdx].pos=w;
        }
        if(heldRIdx>=0){
            V3 w; toWorld(&w,&handR); boxes[heldRIdx].pos=w;
        }
        for(int i=0;i<boxCount;i++)
            if(boxes[i].pos.y < -1.0f+boxes[i].size/2) boxes[i].pos.y=-1.0f+boxes[i].size/2;

        // ---- render ----
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C3D_RenderTargetClear(top, C3D_CLEAR_ALL, CLEAR_COLOR, 0);
        C3D_FrameDrawOn(top);

        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uLoc_projection, &projection);

        C3D_Mtx view; buildView(&view);

        for(int i=0;i<boxCount;i++)
            drawCubeAt(&view, boxes[i].pos, boxes[i].size, boxes[i].col.r, boxes[i].col.g, boxes[i].col.b);

        {
            V3 hl, hr;
            toWorld(&hl, &handL);
            toWorld(&hr, &handR);
            drawCubeAt(&view, hl, 0.16f, 0.5f, 0.9f, 0.5f);
            drawCubeAt(&view, hr, 0.16f, 0.9f, 0.5f, 0.2f);
        }

        // bottom screen stays dark
        C3D_RenderTargetClear(bottom, C3D_CLEAR_ALL, 0xFF202020, 0);
        C3D_FrameDrawOn(bottom);

        C3D_FrameEnd(0);
    }

    shaderProgramFree(&program);
    DVLB_Free(dvlb);
    C3D_Fini();
    gfxExit();
    return 0;
}
