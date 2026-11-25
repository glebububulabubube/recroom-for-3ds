#include <3ds.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>

#include <citro2d.h>

#include "player_model.h"
#include "hand_model.h"
#include "pen_model.h"
#include "room_model.h"
#include "center_model.h"
#include "colliders.h"

// ---------------- music (csnd, looped PCM8 embedded via bin2o) ----------------
// data/menu.bin + data/game.bin become menu_bin/menu_bin_size etc. (bin2o),
// so the tracks are part of the .elf/.cia and always playable.
#include "menu_bin.h"
#include "game_bin.h"
#include "menu_top_bin.h"
#include "menu_bottom_bin.h"
#define MUSIC_CH_MENU 0
#define MUSIC_CH_GAME 1
static u8* musicMenuData = NULL;   // linear-allocated PCM8 samples
static u8* musicGameData = NULL;
static u32 musicMenuSize = 0;
static u32 musicGameSize = 0;
static bool musicInited = false;

static void musicInit(void)
{
    if(musicInited) return;
    if(csndInit() != 0) return;
    // csndChannels is set by csndInit(); channels 0/1 are ours unless blocked

    u32 msz = (u32)(menu_bin_end - menu_bin);
    u32 gsz = (u32)(game_bin_end - game_bin);
    if(msz > 0 && msz <= 0x200000){
        void* buf = linearAlloc(msz + 4);
        if(buf){
            memcpy(buf, menu_bin, msz);
            musicMenuData = (u8*)buf;
            musicMenuSize = msz;
        }
    }
    if(gsz > 0 && gsz <= 0x200000){
        void* buf = linearAlloc(gsz + 4);
        if(buf){
            memcpy(buf, game_bin, gsz);
            musicGameData = (u8*)buf;
            musicGameSize = gsz;
        }
    }

    musicInited = true;
}

static void musicPlayMenu(void)
{
    if(!musicInited || !musicMenuData) return;
    CSND_SetPlayState(MUSIC_CH_MENU, 0);
    csndPlaySound(MUSIC_CH_MENU, SOUND_FORMAT_8BIT | SOUND_REPEAT | SOUND_ENABLE,
                  22050, 0.55f, 0.0f,
                  musicMenuData, musicMenuData, musicMenuSize);
}

static void musicPlayGame(void)
{
    if(!musicInited) return;
    CSND_SetPlayState(MUSIC_CH_MENU, 0); // stop menu track
    if(!musicGameData) return;
    CSND_SetPlayState(MUSIC_CH_GAME, 0);
    csndPlaySound(MUSIC_CH_GAME, SOUND_FORMAT_8BIT | SOUND_REPEAT | SOUND_ENABLE,
                  22050, 0.5f, 0.0f,
                  musicGameData, musicGameData, musicGameSize);
}

static void musicStopAll(void)
{
    if(!musicInited) return;
    CSND_SetPlayState(MUSIC_CH_MENU, 0);
    CSND_SetPlayState(MUSIC_CH_GAME, 0);
}

// ---------------- end music ----------------

// ---------------- menu images (swizzled RGBA8 512x256 via bin2o) ----------------
// The menu background art is stored as 512x256 RGBA8 already swizzled into the
// PICA200 tile order (8x8 tiles, permutations exactly like tex3ds swizzle.cpp)
// by menu_tex.py, so a plain memcpy via C3D_TexUpload shows it correctly.
// Top art occupies 400x240, bottom 320x240.
#define MENU_TEX_W 512
#define MENU_TEX_H 256
extern const u8 menu_top_bin[];
extern const u8 menu_top_bin_end[];
extern const u8 menu_bottom_bin[];
extern const u8 menu_bottom_bin_end[];

static C3D_Tex menuTex[2];   // 0 = top screen art, 1 = bottom screen art
static bool menuTexReady = false;

// sub-texture for the used part of the 512x256 canvas (full UV = whole canvas)
static Tex3DS_SubTexture menuSub[2];
static C2D_Image menuImg[2];

static void menuImagesInit(void)
{
    if(menuTexReady) return;

    const u8* files[2]  = { menu_top_bin, menu_bottom_bin };
    const u8* filesEnd[2] = { menu_top_bin_end, menu_bottom_bin_end };
    // visible art size in the canvas (UV-scaled to the screen later)
    int artW[2] = { 400, 320 };
    int artH[2] = { 240, 240 };
    int i;
    for(i = 0; i < 2; i++){
        size_t sz = (size_t)(filesEnd[i] - files[i]);
        if(sz < MENU_TEX_W * MENU_TEX_H * 4) continue;
        C3D_TexInit(&menuTex[i], MENU_TEX_W, MENU_TEX_H, GPU_RGBA8);
        if(!menuTex[i].data) continue;
        C3D_TexUpload(&menuTex[i], files[i]);   // raw copy, data already swizzled
        C3D_TexFlush(&menuTex[i]);
        C3D_TexSetFilter(&menuTex[i], GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&menuTex[i], GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);

        // field order: width, height, left, top, right, bottom
        // top >= bottom => not rotated; v=1.0 (texel row 0) is the top of the
        // art, matching tex3ds: top=1.0, bottom=1-artH/H
        menuSub[i] = (Tex3DS_SubTexture){
            (u16)artW[i], (u16)artH[i],
            0.0f, 1.0f,
            (float)artW[i] / MENU_TEX_W, 1.0f - (float)artH[i] / MENU_TEX_H
        };
        menuImg[i].tex = &menuTex[i];
        menuImg[i].subtex = &menuSub[i];
    }
    menuTexReady = true;
}

static void menuImagesFree(void)
{
    if(!menuTexReady) return;
    C3D_TexDelete(&menuTex[0]);
    C3D_TexDelete(&menuTex[1]);
    menuTexReady = false;
}

// draw the art stretched over the whole target; which: 0=top, 1=bottom
static void drawMenuImage(int which, int targetW, int targetH)
{
    if(!menuTexReady) return;
    C2D_DrawImageAt(menuImg[which], 0.0f, 0.0f, 0.0f, NULL,
                    (float)targetW / menuSub[which].width,
                    (float)targetH / menuSub[which].height);
}

// ---------------- end menu images ----------------

#define NAME_MAX 16
#define NAME_FILE "sdmc:/recroom3ds_name.txt"

#define SCREEN_W 400
#define SCREEN_H 240
#define FOCAL 220.0f

static char playerName[NAME_MAX + 1] = "";
static C2D_TextBuf g_textBuf = NULL;

static u32 clrOrange;
static u32 clrOrangeD;
static u32 clrWhite;
static u32 clrGrey;

static void initColors(void)
{
    clrOrange  = C2D_Color32(0xFF, 0x6B, 0x0F, 0xFF);
    clrOrangeD = C2D_Color32(0xE8, 0x5A, 0x00, 0xFF);
    clrWhite   = C2D_Color32(0xFF, 0xFF, 0xFF, 0xFF);
    clrGrey    = C2D_Color32(0x80, 0x80, 0x80, 0xFF);
}

#define MENU_ENTRIES 2
static int menuSel = 0;

// ---------------- 3D world ----------------

typedef struct { float x, y, z; } Vec3;

typedef struct {
    Vec3 pos;
    Vec3 size;      // half extents
    u32  color;
    bool grabbed;
    int  grabHand;  // -1 = free
} Box;

typedef struct {
    Vec3 world;
    float roll;     // rotation around view axis: idle hands rest sideways
} Hand;

typedef struct {
    Vec3 pos;
    float yaw, pitch;
    float vy;       // vertical velocity (jump physics)
    bool grounded;  // standing on the floor
} Camera;

// ---- maker pen + air drawing (strokes stay in the world) ----
#define MAX_STROKE 600
typedef struct {
    Vec3 pts[MAX_STROKE];
    int  count;     // points accumulated (0..MAX_STROKE)
    int  write;     // ring buffer write cursor
    Vec3 last;      // last tip position (distance test)
    bool hasLast;
} Stroke;

static Stroke stroke;
static bool  penHeld = false;   // pen in hand
static float penLift = 0.0f;    // pickup animation 0..1 (1 = fully raised)

// a walkable, talkable Rec Room character built from cubes
typedef struct {
    Vec3 pos;       // feet position
    Vec3 target;    // patrol target
    float speed;
    float pause;    // rest timer before next target
    float bound;    // wander radius around the map center
    char name[16];
    int  map;       // which place this bot lives in
    const char* lines[3]; // things it says when talked to
    int  li;        // line cursor
} Bot;

#define MAX_BOTS 14
static Bot bots[MAX_BOTS];

// places: 0 dorm, 1 rec center, 2 chill zone, 3 dudes zone, 4 play room, 5 memorial
static int currentMap = 0;
#define PLACE_COUNT 6
static const char* const PLACE_NAMES[PLACE_COUNT] = {
    "DORM ROOM", "REC CENTER", "CHILL ZONE", "DUDES ZONE", "PLAY ROOM", "REC ROOM MEMORIAL"
};
static bool placesOpen = false; // places menu on the bottom screen
static int  placeSel = 0;

// prop = one solid/decor element on a procedural place
typedef struct {
    float x, z;      // center
    float w, d, h;   // full extents
    float y;         // base height above floor
    u32  col;
    bool solid;      // blocks the player (collider)
} Prop;

#define MAX_PLACE_PROPS 48
#define P_COL(r,g,b) C2D_Color32(r,g,b,0xFF)

// forest props (chill zone) then plaza props (dudes) etc; tables split by map
static const Prop CHILL_PROPS[] = {
    // trunk (solid), foliage (not), placed around a campfire clearing
    { 5.0f,  4.0f, 0.5f, 0.5f, 3.0f, 0.0f, 0x6B4A2BFF, true  },
    { 5.0f,  4.0f, 2.6f, 2.6f, 2.4f, 2.2f, 0x2E7D32FF, false },
    {-6.0f,  5.0f, 0.5f, 0.5f, 3.2f, 0.0f, 0x6B4A2BFF, true  },
    {-6.0f,  5.0f, 2.8f, 2.8f, 2.6f, 2.4f, 0x1F6B27FF, false },
    { 7.0f, -3.0f, 0.5f, 0.5f, 3.0f, 0.0f, 0x6B4A2BFF, true  },
    { 7.0f, -3.0f, 2.5f, 2.5f, 2.3f, 2.1f, 0x2E7D32FF, false },
    {-4.0f, -6.0f, 0.5f, 0.5f, 3.2f, 0.0f, 0x6B4A2BFF, true  },
    {-4.0f, -6.0f, 2.8f, 2.8f, 2.6f, 2.4f, 0x1F6B27FF, false },
    { 9.0f,  6.0f, 0.5f, 0.5f, 3.0f, 0.0f, 0x6B4A2BFF, true  },
    { 9.0f,  6.0f, 2.5f, 2.5f, 2.3f, 2.1f, 0x2E7D32FF, false },
    {-8.0f, -2.0f, 0.5f, 0.5f, 3.2f, 0.0f, 0x6B4A2BFF, true  },
    {-8.0f, -2.0f, 2.8f, 2.8f, 2.6f, 2.4f, 0x1F6B27FF, false },
    { 2.0f, -8.0f, 0.5f, 0.5f, 3.0f, 0.0f, 0x6B4A2BFF, true  },
    { 2.0f, -8.0f, 2.5f, 2.5f, 2.3f, 2.1f, 0x2E7D32FF, false },
    // campfire: stone ring (solid) + logs
    { 0.0f,  0.0f, 1.4f, 1.4f, 0.25f, 0.0f, 0x7A7A7AFF, true },
    { 0.0f,  0.0f, 0.6f, 0.6f, 0.2f, 0.1f, 0x5A3A18FF,  true },
};
static const int CHILL_COUNT = sizeof(CHILL_PROPS)/sizeof(CHILL_PROPS[0]);

static const Prop DUDES_PROPS[] = {
    // fountain in the middle (solid basin) + benches around
    { 0.0f,  0.0f, 2.6f, 2.6f, 0.5f, 0.0f, 0xC9CCD3FF, true },
    { 0.0f,  0.0f, 1.4f, 1.4f, 2.6f, 0.5f, 0x8AD9FFFF, false },
    { 6.0f,  3.0f, 1.6f, 0.6f, 0.6f, 0.0f, 0x8B5A2BFF, true },
    {-6.0f,  3.0f, 1.6f, 0.6f, 0.6f, 0.0f, 0x8B5A2BFF, true },
    { 6.0f, -3.0f, 1.6f, 0.6f, 0.6f, 0.0f, 0x8B5A2BFF, true },
    {-6.0f, -3.0f, 1.6f, 0.6f, 0.6f, 0.0f, 0x8B5A2BFF, true },
    // a couple of crates to jump on
    { 3.0f, 5.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0xC89A4CFF, true },
    {-3.0f, 5.0f, 0.8f, 0.8f, 0.8f, 0.0f, 0xC89A4CFF, true },
};
static const int DUDES_COUNT = sizeof(DUDES_PROPS)/sizeof(DUDES_PROPS[0]);

static const Prop PLAY_PROPS[] = {
    // slide: ladder + chute (boxy)
    { 5.0f, 0.0f, 1.0f, 1.0f, 2.6f, 0.0f, 0xE84C8CFF, true  },
    { 5.0f, 0.0f, 0.9f, 3.6f, 0.25f, 0.8f, 0x4C8BE8FF, false },
    // big colorful blocks
    { -4.0f, 2.0f, 1.2f, 1.2f, 1.2f, 0.0f, 0x5CD85CFF, true },
    { -6.0f, 2.0f, 1.0f, 1.0f, 1.0f, 0.0f, 0xE8D84CFF, true },
    { -5.0f, -2.0f, 1.4f, 1.4f, 0.7f, 0.0f, 0xE88C4CFF, true },
    // ball pit (low box, blue)
    { -2.0f, -4.0f, 3.2f, 3.2f, 0.5f, 0.0f, 0x639BFFFF, true  },
    // spinning top platform
    { 3.0f, -4.0f, 1.0f, 1.0f, 1.2f, 0.0f, 0xA85CE8FF, true },
};
static const int PLAY_COUNT = sizeof(PLAY_PROPS)/sizeof(PLAY_PROPS[0]);

static const Prop MEMORIAL_PROPS[] = {
    // central obelisk + flowers + candles
    { 0.0f, 0.0f, 1.2f, 1.2f, 3.4f, 0.0f, 0xC9CCD3FF, true },
    { 0.0f, 0.0f, 0.7f, 0.7f, 0.5f, 3.2f, 0xFF6B0FFF, true },
    { 2.2f, 1.4f, 0.5f, 0.5f, 0.4f, 0.0f, 0xE84C4CFF, true },
    {-2.2f, 1.4f, 0.5f, 0.5f, 0.4f, 0.0f, 0xE8E84CFF, true },
    { 2.2f,-1.4f, 0.5f, 0.5f, 0.4f, 0.0f, 0xE84CE8FF, true },
    {-2.2f,-1.4f, 0.5f, 0.5f, 0.4f, 0.0f, 0x4CE8D8FF, true },
    { 1.4f, 0.0f, 0.3f, 0.3f, 0.8f, 0.0f, 0xF2F2F2FF, true },
    {-1.4f, 0.0f, 0.3f, 0.3f, 0.8f, 0.0f, 0xF2F2F2FF, true },
};
static const int MEMORIAL_COUNT = sizeof(MEMORIAL_PROPS)/sizeof(MEMORIAL_PROPS[0]);

static const Prop* placeProps(int map, int* outCount)
{
    switch(map){
        case 2: *outCount = CHILL_COUNT;    return CHILL_PROPS;
        case 3: *outCount = DUDES_COUNT;    return DUDES_PROPS;
        case 4: *outCount = PLAY_COUNT;     return PLAY_PROPS;
        case 5: *outCount = MEMORIAL_COUNT; return MEMORIAL_PROPS;
        default: *outCount = 0; return NULL;
    }
}

static Hand handL, handR;
static Camera cam;

// last spoken bot line shown on the bottom screen
static char botMsg[96] = "";
static float botMsgTimer = 0.0f;

typedef struct {
    float depth;
    float x0,y0,x1,y1,x2,y2,x3,y3;
    u32 color;
    bool tri; // true: only 3 verts (x0..x2), no 4th vertex
} Face;

#define MAX_FACES 5500
static Face faces[MAX_FACES];
static int faceCount = 0;

static void buildBotFaces(void); // forward decl: called from buildWorldFaces

static int cmpFaces(const void* a, const void* b)
{
    float d = ((const Face*)b)->depth - ((const Face*)a)->depth;
    return d > 0.0f ? 1 : (d < 0.0f ? -1 : 0);
}

// camera-space projection; returns 0 if behind camera
static float gRYaw, gSYaw, gRPitch, gSPitch; // cached camera trig (set per frame)

static void updateCameraBasis(void)
{
    gRYaw = cosf(cam.yaw);   gSYaw = sinf(cam.yaw);
    gRPitch = cosf(cam.pitch); gSPitch = sinf(cam.pitch);
}

static int project(float x, float y, float z, float* sx, float* sy, float* czOut)
{
    float rx = x - cam.pos.x, ry = y - cam.pos.y, rz = z - cam.pos.z;
    float cY = gRYaw, sY = gSYaw;
    float cP = gRPitch, sP = gSPitch;

    float R[3] = { cY, 0.0f, -sY };
    float U[3] = { sY*sP, -cP, cY*sP };
    float F[3] = { sY*cP, sP, cY*cP };

    float cx = rx*R[0] + ry*R[1] + rz*R[2];
    float cy = rx*U[0] + ry*U[1] + rz*U[2];
    float cz = rx*F[0] + ry*F[1] + rz*F[2];

    if(cz < 0.2f) return 0;
    float inv = FOCAL / cz;
    *sx = SCREEN_W/2.0f + cx*inv;
    *sy = SCREEN_H/2.0f + cy*inv;
    if(czOut) *czOut = cz;
    return 1;
}

static void addFace(const Vec3* v, u32 color, float brightness)
{
    if(faceCount >= MAX_FACES) return;
    float x0,y0,x1,y1,x2,y2,x3,y3;
    float d0,d1,d2,d3;
    if(!project(v[0].x, v[0].y, v[0].z, &x0,&y0,&d0)) return;
    if(!project(v[1].x, v[1].y, v[1].z, &x1,&y1,&d1)) return;
    if(!project(v[2].x, v[2].y, v[2].z, &x2,&y2,&d2)) return;
    if(!project(v[3].x, v[3].y, v[3].z, &x3,&y3,&d3)) return;

    Face* f = &faces[faceCount++];
    f->depth   = (d0+d1+d2+d3)*0.25f;
    f->x0=x0; f->y0=y0; f->x1=x1; f->y1=y1;
    f->x2=x2; f->y2=y2; f->x3=x3; f->y3=y3;
    f->color = C2D_Color32(
        (u8)((color >>  0) & 0xFF) * brightness,
        (u8)((color >>  8) & 0xFF) * brightness,
        (u8)((color >> 16) & 0xFF) * brightness,
        0xFF);
    f->tri = (v[3].x == v[2].x && v[3].y == v[2].y && v[3].z == v[2].z);
}

// add a single triangle (used by glb models) - one draw call, not two.
// cull: skips triangles whose normal points away from the camera (view-space).
static void addTriFaceCull(const Vec3* v, u32 color, bool cull, bool flipWinding)
{
    if(faceCount >= MAX_FACES) return;

    // backface cull in world space: normal vs view vector to tri center
    if(cull){
        float ax=v[0].x, ay=v[0].y, az=v[0].z;
        float bx=v[1].x, by=v[1].y, bz=v[1].z;
        float cx=v[2].x, cy=v[2].y, cz=v[2].z;
        float ux=bx-ax, uy=by-ay, uz=bz-az;
        float vx=cx-ax, vy=cy-ay, vz=cz-az;
        float nx = uy*vz - uz*vy;
        float ny = uz*vx - ux*vz;
        float nz = ux*vy - uy*vx;
        float mx = (ax+bx+cx)/3.0f - cam.pos.x;
        float my = (ay+by+cy)/3.0f - cam.pos.y;
        float mz = (az+bz+cz)/3.0f - cam.pos.z;
        float dot = nx*mx + ny*my + nz*mz;
        if(flipWinding) dot = -dot;
        if(dot >= 0.0f) return; // facing away (or edge-on)
    }

    float x0,y0,x1,y1,x2,y2;
    float d0,d1,d2;
    if(!project(v[0].x, v[0].y, v[0].z, &x0,&y0,&d0)) return;
    if(!project(v[1].x, v[1].y, v[1].z, &x1,&y1,&d1)) return;
    if(!project(v[2].x, v[2].y, v[2].z, &x2,&y2,&d2)) return;

    Face* f = &faces[faceCount++];
    f->depth   = (d0+d1+d2)/3.0f;
    f->x0=x0; f->y0=y0; f->x1=x1; f->y1=y1;
    f->x2=x2; f->y2=y2;
    f->color = color;
    f->tri = true;
}

static void drawFaceQuad(const Face* f)
{
    if(f->tri){
        C2D_DrawTriangle(f->x0,f->y0,f->color,
                         f->x1,f->y1,f->color,
                         f->x2,f->y2,f->color, 0.5f);
        return;
    }
    C2D_DrawTriangle(f->x0,f->y0,f->color,
                     f->x1,f->y1,f->color,
                     f->x2,f->y2,f->color, 0.5f);
    C2D_DrawTriangle(f->x0,f->y0,f->color,
                     f->x2,f->y2,f->color,
                     f->x3,f->y3,f->color, 0.5f);
}

static void addPlaceProp(const Prop* p)
{
    float hx = p->w*0.5f, hy = p->h*0.5f, hz = p->d*0.5f;
    float cy = p->y + hy;
    Vec3 c[8] = {
        { p->x-hx, cy-hy, p->z-hz }, { p->x+hx, cy-hy, p->z-hz },
        { p->x+hx, cy-hy, p->z+hz }, { p->x-hx, cy-hy, p->z+hz },
        { p->x-hx, cy+hy, p->z-hz }, { p->x+hx, cy+hy, p->z-hz },
        { p->x+hx, cy+hy, p->z+hz }, { p->x-hx, cy+hy, p->z+hz }
    };
    Vec3 tmp[4];
    tmp[0]=c[0]; tmp[1]=c[1]; tmp[2]=c[2]; tmp[3]=c[3];
    addFace(tmp, p->col, 0.55f);
    tmp[0]=c[4]; tmp[1]=c[6]; tmp[2]=c[5]; tmp[3]=c[7];
    addFace(tmp, p->col, 1.0f);
    tmp[0]=c[0]; tmp[1]=c[4]; tmp[2]=c[5]; tmp[3]=c[1];
    addFace(tmp, p->col, 0.75f);
    tmp[0]=c[3]; tmp[1]=c[2]; tmp[2]=c[6]; tmp[3]=c[7];
    addFace(tmp, p->col, 0.85f);
    tmp[0]=c[0]; tmp[1]=c[3]; tmp[2]=c[7]; tmp[3]=c[4];
    addFace(tmp, p->col, 0.65f);
    tmp[0]=c[1]; tmp[1]=c[5]; tmp[2]=c[6]; tmp[3]=c[2];
    addFace(tmp, p->col, 0.7f);
}

// draw the real dorm room from DormRoom v1.glb (room_model.h)
static void addRoomModelFaces(void)
{
    int pn;
    for(pn = 0; pn < RM_PART_COUNT; pn++){
        const RM_Part* part = &RM_PARTS[pn];
        u32 col = C2D_Color32((u8)(part->r*255.0f), (u8)(part->g*255.0f), (u8)(part->b*255.0f), 0xFF);
        int i;
        for(i = 0; i < part->faceCount * 9; i += 9){
            Vec3 v[3];
            int vi;
            for(vi = 0; vi < 3; vi++){
                v[vi].x = part->data[i + vi*3 + 0];
                v[vi].y = part->data[i + vi*3 + 1];
                v[vi].z = part->data[i + vi*3 + 2];
            }
            addTriFaceCull(v, col, part->cull, false);
        }
    }
}

// draw the big Rec Center map (Scene.glb, center_model.h)
static void addCenterModelFaces(void)
{
    int pn;
    for(pn = 0; pn < CM_PART_COUNT; pn++){
        const CM_Part* part = &CM_PARTS[pn];
        u32 col = C2D_Color32((u8)(part->r*255.0f), (u8)(part->g*255.0f), (u8)(part->b*255.0f), 0xFF);
        int i;
        for(i = 0; i < part->faceCount * 9; i += 9){
            Vec3 v[3];
            int vi;
            for(vi = 0; vi < 3; vi++){
                v[vi].x = part->data[i + vi*3 + 0];
                v[vi].y = part->data[i + vi*3 + 1];
                v[vi].z = part->data[i + vi*3 + 2];
            }
            addTriFaceCull(v, col, part->cull, false);
        }
    }
}

// add a real Rec Room glove model (hand_model.h) at hand position/orientation.
// handPart: 0 = right glove (as authored), 1 = left glove (mirrored on X).
// roll: rotation around the view axis (idle hands lie sideways, palm in).
// squeeze: 0..1 fist-closing fraction (the hand curls around the pen).
static void addHandFaces(const Box* h, int handPart, float roll, float squeeze)
{
    const HM_Part* part = &HM_PARTS[handPart];
    const float S = 0.45f;
    u32 col = C2D_Color32((u8)(part->r*255.0f), (u8)(part->g*255.0f), (u8)(part->b*255.0f), 0xFF);

    float cY = gRYaw, sY = gSYaw;
    float cP = cosf(cam.pitch), sP = sinf(cam.pitch);
    float R[3] = { cY, 0.0f, -sY };
    float U[3] = { sY*sP, -cP, cY*sP };
    float F[3] = { sY*cP, sP, cY*cP };

    float cr = cosf(roll), sr = sinf(roll);
    float Rr[3] = { R[0]*cr + U[0]*sr, R[1]*cr + U[1]*sr, R[2]*cr + U[2]*sr };
    float Ur[3] = { -R[0]*sr + U[0]*cr, -R[1]*sr + U[1]*cr, -R[2]*sr + U[2]*cr };

    // fingers live near local y = +0.64; bend them forward over the fist
    const float curl = 1.4f * squeeze;
    const float hingeY = 0.30f;
    const float palmPush = 0.08f * squeeze;
    float cc = cosf(curl), sc = sinf(curl);

    int i;
    for(i = 0; i < part->faceCount * 9; i += 9){
        Vec3 v[3];
        int vi;
        for(vi = 0; vi < 3; vi++){
            float lx = part->data[i + vi*3 + 0] * S;
            float ly = part->data[i + vi*3 + 1] * S;
            float lz = part->data[i + vi*3 + 2] * S;
            if(ly > hingeY*S){
                float dy = ly - hingeY*S;
                float ny = hingeY*S + dy*cc - lz*sc;
                float nz = dy*sc + lz*cc;
                ly = ny;
                lz = nz - palmPush*S;
            }
            v[vi].x = lx*Rr[0] + ly*Ur[0] + lz*F[0] + h->pos.x;
            v[vi].y = lx*Rr[1] + ly*Ur[1] + lz*F[1] + h->pos.y;
            v[vi].z = lx*Rr[2] + ly*Ur[2] + lz*F[2] + h->pos.z;
        }
        // left hand (part 1) is mirrored on X, which flips the winding
        addTriFaceCull(v, col, true, handPart == 1);
    }
}

// draw the Maker Pen (bigger gun, no glove) at hand position.
// Pen's long axis is local Y; the orange tip is at NEGATIVE local Y, so
// mapping ly -> -F points the tip away from the camera. x->R, z->U, y->-F.
static void addPenFaces(const Box* h)
{
    const float S = 0.65f; // pen gun: bigger, no glove around it
    int pn;
    for(pn = 0; pn < PEN_PART_COUNT; pn++){
        const PEN_Part* part = &PEN_PARTS[pn];
        u32 col = C2D_Color32((u8)(part->r*255.0f), (u8)(part->g*255.0f), (u8)(part->b*255.0f), 0xFF);
        float cY = gRYaw, sY = gSYaw;
        float cP = cosf(cam.pitch), sP = sinf(cam.pitch);
        float R[3] = { cY, 0.0f, -sY };
        float U[3] = { sY*sP, -cP, cY*sP };
        float F[3] = { sY*cP, sP, cY*cP };

        int i;
        for(i = 0; i < part->faceCount * 9; i += 9){
            Vec3 v[3];
            int vi;
            for(vi = 0; vi < 3; vi++){
                float lx = part->data[i + vi*3 + 0] * S;
                float ly = part->data[i + vi*3 + 1] * S;
                float lz = part->data[i + vi*3 + 2] * S;
                v[vi].x = lx*R[0] + lz*U[0] - ly*F[0] + h->pos.x;
                v[vi].y = lx*R[1] + lz*U[1] - ly*F[1] + h->pos.y;
                v[vi].z = lx*R[2] + lz*U[2] - ly*F[2] + h->pos.z;
            }
            addTriFaceCull(v, col, false, false); // pen is small, draw both sides
        }
    }
}

// orange air-drawing stroke: a polyline of short camera-facing quads
static void addStrokeFaces(void)
{
    if(stroke.count < 2) return;
    int i;
    for(i = 0; i < stroke.count - 1; i++){
        Vec3 a = stroke.pts[i];
        Vec3 b = stroke.pts[i+1];
        Vec3 d = { b.x-a.x, b.y-a.y, b.z-a.z };
        float len = sqrtf(d.x*d.x + d.y*d.y + d.z*d.z);
        if(len < 0.001f) continue;
        u32 col = C2D_Color32(0xFF, 0x8C, 0x1A, 0xFF); // Rec Room orange
        Vec3 halfR = { gRYaw*0.012f, 0.0f, -gSYaw*0.012f };
        Vec3 halfU = { gSYaw*gSPitch*0.012f, -gRPitch*0.012f, gRYaw*gSPitch*0.012f };
        Vec3 q[4] = {
            { a.x - halfR.x - halfU.x, a.y - halfR.y - halfU.y, a.z - halfR.z - halfU.z },
            { a.x + halfR.x - halfU.x, a.y + halfR.y - halfU.y, a.z + halfR.z - halfU.z },
            { b.x + halfR.x + halfU.x, b.y + halfR.y + halfU.y, b.z + halfR.z + halfU.z },
            { b.x - halfR.x + halfU.x, b.y - halfR.y + halfU.y, b.z - halfR.z + halfU.z }
        };
        addFace(q, col, 1.0f);
    }
}

static void buildWorldFaces(void)
{
    faceCount = 0;

    if(currentMap == 0){
        // real dorm room shell + furniture from DormRoom v1.glb (room_model.h)
        addRoomModelFaces();
    } else if(currentMap == 1){
        // big Rec Center map (Scene.glb, center_model.h)
        addCenterModelFaces();
    } else {
        // procedural places: chill zone / dudes zone / play room / memorial
        u32 ground = 0x2E4A2EFF; // grass green default
        if(currentMap == 3)      ground = 0x8F8F96FF; // stone plaza
        else if(currentMap == 4) ground = 0xD8B47EFF; // wood deck
        else if(currentMap == 5) ground = 0x6B6B72FF; // slate
        Vec3 v[4] = {
            { -35.0f, 0.0f, -35.0f }, { 35.0f, 0.0f, -35.0f },
            { 35.0f, 0.0f, 35.0f },   { -35.0f, 0.0f, 35.0f }
        };
        addFace(v, ground, 1.0f);
        int n = 0;
        const Prop* props = placeProps(currentMap, &n);
        int i;
        for(i = 0; i < n; i++)
            addPlaceProp(&props[i]);
    }

    // hands
    {
        Box h;
        h.color = C2D_Color32(0xFF,0xFF,0xFF,0xFF);
        h.size.x=0.12f; h.size.y=0.12f; h.size.z=0.12f;
        h.pos = handL.world;
        addHandFaces(&h, 1, handL.roll, 0.0f); // left hand mirrored part
        h.pos = handR.world;
        if(penHeld){
            // pen gun in hand: draw only the gun (bigger), no glove around it
            addPenFaces(&h);
        } else {
            addHandFaces(&h, 0, handR.roll, 0.0f); // right hand as-is
        }
    }

    // air-drawn stroke lives in the world; draw it every frame
    addStrokeFaces();

    // bots live in the Rec Center and the Dudes Zone
    if(currentMap == 1 || currentMap == 3)
        buildBotFaces(); // avatar triangles join the depth-sorted queue

    qsort(faces, faceCount, sizeof(Face), cmpFaces);
}

static void moveHandTo(Hand* h, float tx, float ty, float tz)
{
    h->world.x += (tx - h->world.x) * 0.3f;
    h->world.y += (ty - h->world.y) * 0.3f;
    h->world.z += (tz - h->world.z) * 0.3f;
}

// ---------------- bots: patrol + talk ----------------

static void initBots(void)
{
    memset(bots, 0, sizeof(bots));

    // Rec Center wanderers (map 1)
    {
        Bot* b = &bots[0];
        strncpy(b->name, "Emperor", 15);
        b->map = 1; b->speed = 1.2f;
        b->pos.x = 1.2f; b->pos.z = -1.5f;
        b->target.x = -0.8f; b->target.z = -2.2f;
        b->bound = 24.0f;
        b->lines[0] = "Let's build the new Rec Room!";
        b->lines[1] = "The Rec Center never sleeps.";
        b->lines[2] = "Try the new places button!";
    }
    {
        Bot* b = &bots[1];
        strncpy(b->name, "Runner", 15);
        b->map = 1; b->speed = 1.5f;
        b->pos.x = -3.0f; b->pos.z = 4.0f;
        b->target.x = 2.0f; b->target.z = -3.0f;
        b->bound = 26.0f;
        b->lines[0] = "I run laps around the center.";
        b->lines[1] = "Race you to the door!";
        b->lines[2] = "Gotta keep moving.";
    }
    {
        Bot* b = &bots[2];
        strncpy(b->name, "Builder", 15);
        b->map = 1; b->speed = 0.8f;
        b->pos.x = 5.0f; b->pos.z = 6.0f;
        b->target.x = -5.0f; b->target.z = 6.0f;
        b->bound = 20.0f;
        b->lines[0] = "This place needs more rooms.";
        b->lines[1] = "I heard there's a forest zone.";
        b->lines[2] = "Memorial Day is special.";
    }

    // Dudes Zone (map 3): a crowd of NPCs, each with its own lines
    {
        static const struct { const char* n; float x, z; const char* l0, *l1, *l2; } dudes[] = {
            { "Chill Dude",  -4.0f,  3.0f, "Hey, man. Take it easy.", "The fire is warm tonight.", "Rec Room forever, bro." },
            { "DJ Dude",      4.0f,  2.5f, "Wanna hear a beat?", "This plaza has great acoustics.", "Turn it up!" },
            { "Builder Dude",-5.0f, -2.5f, "I'm building a castle.", "Blocks > drama.", "Check out my blueprints." },
            { "Artist Dude",  5.0f, -3.0f, "The pen gun is my brush.", "Doodle in the air!", "Colors tell stories." },
            { "Gamer Dude",  -3.0f, -5.0f, "Pong is life.", "GG, that was close.", "One more round!" },
            { "Ski Dude",     3.0f, -6.0f, "I miss the slopes.", "Ride fast, fall hard.", "Powder days forever." },
            { "Partier Dude", 0.0f,  6.0f, "Let's celebrate!", "Balloons everywhere!", "You are invited." },
            { "Scuba Dude",  -6.0f,  5.0f, "The deep is peaceful.", "I found a coral reef once.", "Don't hold your breath." },
            { "Boss Dude",   -2.0f,  2.0f, "The zone needs a leader.", "Stay sharp, dude.", "Respect the fountain." },
            { "New Dude",     2.0f, -3.0f, "I just started playing.", "How do I draw again?", "Cool place, right?" },
        };
        int k;
        for(k = 0; k < 10; k++){
            Bot* b = &bots[3 + k];
            strncpy(b->name, dudes[k].n, 15);
            b->map = 3;
            b->speed = 0.4f;           // mostly standing, small shuffles
            b->pos.x = dudes[k].x; b->pos.z = dudes[k].z;
            b->target.x = dudes[k].x + 0.8f; b->target.z = dudes[k].z + 0.5f;
            b->bound = 2.0f;
            b->lines[0] = dudes[k].l0;
            b->lines[1] = dudes[k].l1;
            b->lines[2] = dudes[k].l2;
        }
    }
}

static void setBotMsg(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(botMsg, sizeof(botMsg), fmt, args);
    va_end(args);
    botMsgTimer = 6.0f;
}

// talk: closest bot on THIS map within 3.5 units says one of its own lines
static void tryTalk(void)
{
    int best = -1;
    float bestDist = 3.5f * 3.5f;
    int i;
    for(i = 0; i < MAX_BOTS; i++){
        if(bots[i].map != currentMap) continue;
        float dx = bots[i].pos.x - cam.pos.x;
        float dz = bots[i].pos.z - cam.pos.z;
        float d = dx*dx + dz*dz;
        if(d < bestDist){ bestDist = d; best = i; }
    }
    if(best < 0){
        setBotMsg("Walk closer to a bot to talk!");
        return;
    }
    Bot* b = &bots[best];
    setBotMsg("%s: %s", b->name, b->lines[b->li]);
    b->li = (b->li + 1) % 3;   // next line next time
}

static void updateBots(float dt)
{
    int i;
    for(i = 0; i < MAX_BOTS; i++){
        Bot* b = &bots[i];
        if(b->pause > 0.0f){
            b->pause -= dt;
            continue;
        }
        float dx = b->target.x - b->pos.x;
        float dz = b->target.z - b->pos.z;
        float dist = sqrtf(dx*dx + dz*dz);
        if(dist < 0.15f){
            // arrived: pick a new target and pause (wandering the Rec Center)
            b->pause = 1.2f + (float)(i * 0.4f);
            b->target.x = ((float)(rand() % 320) - 160.0f) / 10.0f;
            b->target.z = ((float)(rand() % 320) - 160.0f) / 10.0f;
            if(b->target.x >  24.0f) b->target.x =  24.0f;
            if(b->target.z >  24.0f) b->target.z =  24.0f;
            if(b->target.x < -24.0f) b->target.x = -24.0f;
            if(b->target.z < -24.0f) b->target.z = -24.0f;
        } else {
            float inv = b->speed * dt / dist;
            b->pos.x += dx * inv;
            b->pos.z += dz * inv;
        }
        // keep bot on the floor
        b->pos.y = 0.0f;
    }
    if(botMsgTimer > 0.0f) botMsgTimer -= dt;
}

// draw a small flat feature (eye/mouth) on the head, model-local coords,
// rotated with the bot heading like the rest of the avatar.
static void addFaceFeature(float cx, float cy, float hx, float hy, float fz,
                           const Bot* b, float ca, float sa)
{
    Vec3 q[4];
    float lx[4] = { cx-hx, cx+hx, cx+hx, cx-hx };
    float ly[4] = { cy-hy, cy-hy, cy+hy, cy+hy };
    int k;
    for(k = 0; k < 4; k++){
        q[k].x = lx[k]*ca - fz*sa + b->pos.x;
        q[k].y = ly[k] + b->pos.y;
        q[k].z = lx[k]*sa + fz*ca + b->pos.z;
    }
    addFace(q, C2D_Color32(0x18,0x10,0x0A,0xFF), 1.0f);
}

// render the real Rec Room avatar (from player_model.h) at feet position p
static void drawBot(const Bot* b)
{
    // model fills roughly xz=±0.5, y 0..1.7; place feet at b->pos
    float dx = b->target.x - b->pos.x;
    float dz = b->target.z - b->pos.z;
    float ang = atan2f(dx, dz); // heading to face movement direction
    float ca = cosf(ang), sa = sinf(ang);

    int pn;
    for(pn = 0; pn < PM_PART_COUNT; pn++){
        const PM_Part* part = &PM_PARTS[pn];
        u32 col = C2D_Color32((u8)(part->r*255.0f), (u8)(part->g*255.0f), (u8)(part->b*255.0f), 0xFF);
        int i;
        for(i = 0; i < part->faceCount * 9; i += 9){
            Vec3 v[3];
            int vi;
            for(vi = 0; vi < 3; vi++){
                float lx = part->data[i + vi*3 + 0];
                float ly = part->data[i + vi*3 + 1];
                float lz = part->data[i + vi*3 + 2];
                v[vi].x = lx*ca - lz*sa + b->pos.x;
                v[vi].y = ly + b->pos.y;
                v[vi].z = lx*sa + lz*ca + b->pos.z;
            }
            // part 0 (hair) is an open shell - draw both sides; parts 1/3 are solid
            addTriFaceCull(v, col, (pn == 1 || pn == 3), false);
        }
    }

    // guarantee visible features: two eyes + smile on the head
    float fz = 0.160f;                      // face plane: beyond skin surface (max z=0.138)
    addFaceFeature(-0.045f, 1.55f, 0.030f, 0.035f, fz, b, ca, sa); // left eye
    addFaceFeature( 0.045f, 1.55f, 0.030f, 0.035f, fz, b, ca, sa); // right eye
    addFaceFeature( 0.000f, 1.46f, 0.030f, 0.008f, fz, b, ca, sa); // smile
}

static void buildBotFaces(void)
{
    int i;
    for(i = 0; i < MAX_BOTS; i++)
        if(bots[i].map == currentMap) // only bots of the current place
            drawBot(&bots[i]);
}

static void updateHands(void)
{
    u32 held = hidKeysHeld();
    circlePosition pad;
    hidCircleRead(&pad);

    float dx = pad.dx / 156.0f, dy = pad.dy / 156.0f;
    if(dx > 1.0f) dx = 1.0f;
    if(dx < -1.0f) dx = -1.0f;
    if(dy > 1.0f) dy = 1.0f;
    if(dy < -1.0f) dy = -1.0f;
    if(fabsf(dx) < 0.15f) dx = 0.0f;
    if(fabsf(dy) < 0.15f) dy = 0.0f;

    float cY = gRYaw, sY = gSYaw;
    float cP = gRPitch, sP = gSPitch;
    float R[3] = { cY, 0.0f, -sY };
    float U[3] = { sY*sP, -cP, cY*sP };
    float F[3] = { sY*cP, sP, cY*cP };

    float px = cam.pos.x, py = cam.pos.y, pz = cam.pos.z;

    // with the pen in hand, the right hand is raised forward-up to aim
    float penRaise = penHeld ? (0.55f + 0.45f * penLift) : 0.0f; // 0.55 idle -> 1.0 raised

    float txL, tyL, tzL, txR, tyR, tzR;
    float rollL, rollR;

    if(held & KEY_L){
        // left hand follows stick (reaches forward, palm away from camera)
        txL = px + R[0]*(dx*1.0f) + F[0]*1.4f + U[0]*(-dy*0.7f);
        tyL = py + R[1]*(dx*1.0f) + F[1]*1.4f + U[1]*(-dy*0.7f);
        tzL = pz + R[2]*(dx*1.0f) + F[2]*1.4f + U[2]*(-dy*0.7f);
        rollL = 0.0f;
    } else {
        // idle: hang low-left, turned sideways (palm in); visible when you look down
        txL = px + R[0]*(-0.38f) + F[0]*0.55f + U[0]*1.05f;
        tyL = py + R[1]*(-0.38f) + F[1]*0.55f + U[1]*1.05f;
        tzL = pz + R[2]*(-0.38f) + F[2]*0.55f + U[2]*1.05f;
        rollL = 1.5708f; // turn sideways, palm facing in
    }

    if(penHeld && penLift < 0.99f){
        // pickup animation: smoothly raise the hand to aim. The fist closes
        // in buildWorldFaces via the squeeze term. Stick control waits for the
        // full raise, so grabbing the pen is always visibly animated.
        txR = px + R[0]*0.18f + F[0]*penRaise*1.2f + U[0]*penRaise*0.55f;
        tyR = py + R[1]*0.18f + F[1]*penRaise*1.2f + U[1]*penRaise*0.55f;
        tzR = pz + R[2]*0.18f + F[2]*penRaise*1.2f + U[2]*penRaise*0.55f;
        rollR = 0.35f;   // slight rotation for a natural "holding a pen" pose
    } else if((held & KEY_R) || penHeld){
        // fully raised: the stick aims the pen (drawing happens while R is held)
        float reach = penHeld ? (1.0f + 0.4f * penLift) : 1.0f;
        txR = px + R[0]*(dx*1.0f) + F[0]*(1.4f*reach) + U[0]*(-dy*0.7f);
        tyR = py + R[1]*(dx*1.0f) + F[1]*(1.4f*reach) + U[1]*(-dy*0.7f);
        tzR = pz + R[2]*(dx*1.0f) + F[2]*(1.4f*reach) + U[2]*(-dy*0.7f);
        rollR = 0.0f;
    } else {
        // idle: hang low-right, turned sideways
        txR = px + R[0]*(0.38f) + F[0]*0.55f + U[0]*1.05f;
        tyR = py + R[1]*(0.38f) + F[1]*0.55f + U[1]*1.05f;
        tzR = pz + R[2]*(0.38f) + F[2]*0.55f + U[2]*1.05f;
        rollR = -1.5708f; // turn sideways, palm facing in
    }

    moveHandTo(&handL, txL, tyL, tzL);
    moveHandTo(&handR, txR, tyR, tzR);
    handL.roll += (rollL - handL.roll) * 0.25f;
    handR.roll += (rollR - handR.roll) * 0.25f;
}

static float mapFloorY(void);   // fwd: used by teleportToPlace below
static float mapEyeY(void);

// teleport to a place from any other; keeps the player on the floor
static void teleportToPlace(int place, int fromPlace)
{
    if(place == 1){ // rec center: enter near the middle, facing back
        cam.pos.x = 0.0f; cam.pos.z = 0.0f;
        cam.yaw = 0.0f; cam.pitch = 0.0f;
    } else if(place == 3){ // dudes zone: face the fountain
        cam.pos.x = 0.0f; cam.pos.z = 6.0f;
        cam.yaw = 3.14159f; cam.pitch = 0.0f;
    } else if(place == 2){ // chill zone: near the campfire
        cam.pos.x = 3.0f; cam.pos.z = 4.0f;
        cam.yaw = -1.2f; cam.pitch = 0.0f;
    } else if(place == 4){ // play room
        cam.pos.x = 0.0f; cam.pos.z = 6.0f;
        cam.yaw = 3.14159f; cam.pitch = 0.0f;
    } else if(place == 5){ // memorial
        cam.pos.x = 0.0f; cam.pos.z = 6.0f;
        cam.yaw = 3.14159f; cam.pitch = 0.0f;
    } else { // dorm
        cam.pos.x = 0.0f; cam.pos.z = 1.0f;
        cam.yaw = 3.14159f; cam.pitch = 0.0f;
    }
    cam.pos.y = mapEyeY(); cam.vy = 0.0f; cam.grounded = true;
    if(place != fromPlace){
        botMsg[0] = 0;
    }
}

// floor height per map: dorm room floor y=0, Rec Center y=-2.5, procedural places y=0
static float mapFloorY(void)
{
    return currentMap == 1 ? -2.5f : 0.0f;
}
// eye height above the map's floor
static float mapEyeY(void)
{
    return mapFloorY() + 1.3f;
}

// player is a vertical capsule ~0.35m radius; walk into colliders is pushed out.
// colliders.h holds the solid geometry (walls, furniture, columns).
static void collidePlayer(void)
{
    const float R = 0.35f;
    const ColBox* list = currentMap == 0 ? ROOM_COLLIDERS : CENTER_COLLIDERS;
    int n = currentMap == 0 ? ROOM_COLLIDER_COUNT : CENTER_COLLIDER_COUNT;
    int i;
    for(i = 0; i < n; i++){
        const ColBox* b = &list[i];
        // player's eye (cam.pos) is ~1.3 above feet; test the capsule against the box
        float p0y = cam.pos.y - 1.3f + 0.05f;   // capsule bottom
        float p1y = cam.pos.y + 0.6f;           // capsule top (chest)
        if(b->maxY < p0y || b->minY > p1y - 0.15f)
            continue;
        // closest point on the box to the player's XZ
        float cx = fminf(fmaxf(cam.pos.x, b->minX), b->maxX);
        float cz = fminf(fmaxf(cam.pos.z, b->minZ), b->maxZ);
        float dx = cam.pos.x - cx, dz = cam.pos.z - cz;
        float d2 = dx*dx + dz*dz;
        if(d2 >= R*R || d2 == 0.0f)
            continue;
        // resolve: push out along the shortest axis
        float d = sqrtf(d2);
        cam.pos.x = cx + dx * (R/d);
        cam.pos.z = cz + dz * (R/d);
    }

    // procedural places: solid props (trees, benches, fountain, crates...)
    if(currentMap >= 2){
        int np = 0;
        const Prop* props = placeProps(currentMap, &np);
        for(i = 0; i < np; i++){
            const Prop* p = &props[i];
            if(!p->solid) continue;
            float p0y = cam.pos.y - 1.3f + 0.05f;
            float p1y = cam.pos.y + 0.6f;
            if(p->y + p->h < p0y || p->y > p1y - 0.15f)
                continue;
            float minX = p->x - p->w*0.5f, maxX = p->x + p->w*0.5f;
            float minZ = p->z - p->d*0.5f, maxZ = p->z + p->d*0.5f;
            float cx = fminf(fmaxf(cam.pos.x, minX), maxX);
            float cz = fminf(fmaxf(cam.pos.z, minZ), maxZ);
            float dx = cam.pos.x - cx, dz = cam.pos.z - cz;
            float d2 = dx*dx + dz*dz;
            if(d2 >= R*R || d2 == 0.0f)
                continue;
            float d = sqrtf(d2);
            cam.pos.x = cx + dx * (R/d);
            cam.pos.z = cz + dz * (R/d);
        }
    }
}

static void updatePlayer(float dt)
{
    u32 held = hidKeysHeld();
    circlePosition pad;
    hidCircleRead(&pad);

    float dx = pad.dx / 156.0f, dy = pad.dy / 156.0f;
    if(dx > 1.0f) dx = 1.0f;
    if(dx < -1.0f) dx = -1.0f;
    if(dy > 1.0f) dy = 1.0f;
    if(dy < -1.0f) dy = -1.0f;
    if(fabsf(dx) < 0.15f) dx = 0.0f;
    if(fabsf(dy) < 0.15f) dy = 0.0f;

    // camera follows the PHYSICAL position of the ABXY buttons on 3DS.
    // pitch+ raises the view (view dir Y = sin(pitch) > 0 when pitching up),
    // so X (top) must increase pitch and B (bottom) must decrease it.
    // Y (left) -> look left, A (right) -> look right.
    if(held & KEY_X) cam.pitch  = fminf(cam.pitch + 2.5f * dt,  1.4f);
    if(held & KEY_B) cam.pitch  = fmaxf(cam.pitch - 2.5f * dt, -1.4f);
    if(held & KEY_Y) cam.yaw   -= 2.5f * dt;
    if(held & KEY_A) cam.yaw   += 2.5f * dt;

    // walking only when hands are not moving (L/R not held)
    if(!(held & (KEY_L | KEY_R))){
        float cY = gRYaw, sY = gSYaw;
        float speed = 5.0f * dt;
        cam.pos.x += (cY*dx + sY*dy) * speed;
        cam.pos.z += (-sY*dx + cY*dy) * speed;
    }

    if(currentMap == 0){
        // dorm room: door leads to the Rec Center (mirrored door at x~-4.5, z~-1.2)
        if(cam.pos.x < -4.3f && cam.pos.z > -1.9f && cam.pos.z < -0.5f){
            int from = currentMap;
            currentMap = 1;
            teleportToPlace(currentMap, from);
            initBots();
            botMsg[0] = 0;
        }
        // clamping inside the real room bounds (DormRoom v1: x[-5.1..4.9], z[-3.4..2.8])
        if(cam.pos.x < -4.9f) cam.pos.x = -4.9f;
        if(cam.pos.x >  4.7f) cam.pos.x =  4.7f;
        if(cam.pos.z < -3.2f) cam.pos.z = -3.2f;
        if(cam.pos.z >  2.6f) cam.pos.z =  2.6f;
    } else if(currentMap == 1){
        // Rec Center: portal back near the middle of the map
        if(cam.pos.x < -30.0f || cam.pos.x > 30.0f ||
           cam.pos.z < -34.0f || cam.pos.z > 34.0f){
            int from = currentMap;
            currentMap = 0;
            teleportToPlace(currentMap, from);
            botMsg[0] = 0;
        }
    } else {
        // procedural places: walk off the ground edge to return to the dorm
        if(cam.pos.x < -36.0f || cam.pos.x > 36.0f ||
           cam.pos.z < -36.0f || cam.pos.z > 36.0f){
            int from = currentMap;
            currentMap = 0;
            teleportToPlace(currentMap, from);
            botMsg[0] = 0;
        }
    }

    // solid geometry: push the player out of walls/furniture/columns
    collidePlayer();

    {
        touchPosition touch;
        hidTouchRead(&touch);
        if(hidKeysDown() & KEY_TOUCH){
            if(placesOpen){
                // places menu: tap a row to travel there; tap outside closes it
                float ty = touch.py;
                if(ty >= 62.0f && ty < 62.0f + PLACE_COUNT * 24.0f){
                    int idx = (int)((ty - 62.0f) / 24.0f);
                    if(idx >= 0 && idx < PLACE_COUNT){
                        int from = currentMap;
                        currentMap = idx;
                        teleportToPlace(currentMap, from);
                        initBots();
                        botMsg[0] = 0;
                        placesOpen = false;
                        stroke.count = 0;   // take the pen away when traveling
                        penHeld = false;
                        penLift = 0.0f;
                    }
                } else {
                    placesOpen = false;
                }
            } else {
                // PEN button: toggle the Maker Pen (bottom screen, center-right)
                float tdx = touch.px - 200.0f, tdy = touch.py - 198.0f;
                if(tdx*tdx + tdy*tdy < 32.0f*32.0f){
                    penHeld = !penHeld;
                    if(!penHeld) stroke.count = 0; // put the pen away: clear the line
                }
                // PLACES button: open the place list (bottom screen, left)
                float pdx = touch.px - 130.0f, pdy = touch.py - 198.0f;
                if(pdx*pdx + pdy*pdy < 32.0f*32.0f){
                    placesOpen = true;
                    placeSel = currentMap;
                }
            }
        }
        // jump: touch the JUMP button (only when the places menu is closed)
        if(!placesOpen && (hidKeysDown() & KEY_TOUCH) && cam.grounded){
            float jdx = touch.px - 270.0f, jdy = touch.py - 198.0f;
            if(jdx*jdx + jdy*jdy < 32.0f*32.0f){
                cam.vy = 7.0f;   // ~1.3 m jump with gravity 18
                cam.grounded = false;
            }
        }
    }

    // pickup/lower animation: penLift eases 0..1 over ~0.5s (visible raise)
    if(penHeld){
        penLift += 2.2f * dt;
        if(penLift > 1.0f) penLift = 1.0f;
    } else {
        penLift -= 2.2f * dt;
        if(penLift < 0.0f) penLift = 0.0f;
    }

    // air drawing: while the pen is in hand, raised, AND the right trigger (R)
    // is held, drop ink points from the pen tip as the stick moves the hand.
    // Points closer than 4cm are skipped.
    u32 heldNow = hidKeysHeld();
    if(penHeld && penLift > 0.85f && (heldNow & KEY_R)){
        Vec3 tip;
        float cY = gRYaw, sY = gSYaw;
        float cP = gRPitch, sP = gSPitch;
        Vec3 F = { sY*cP, sP, cY*cP };
        // pen tip: ~0.12m in front of the right hand along the view
        tip.x = handR.world.x + F.x*0.12f;
        tip.y = handR.world.y + F.y*0.12f;
        tip.z = handR.world.z + F.z*0.12f;
        if(!stroke.hasLast){
            stroke.pts[0] = tip;
            stroke.count = 1;
            stroke.write = 1;
            stroke.last = tip;
            stroke.hasLast = true;
        } else {
            float dx = tip.x - stroke.last.x;
            float dy = tip.y - stroke.last.y;
            float dz = tip.z - stroke.last.z;
            if(dx*dx + dy*dy + dz*dz > 0.04f*0.04f){
                if(stroke.count < MAX_STROKE)
                    stroke.pts[stroke.count++] = tip;
                else
                    stroke.pts[stroke.write] = tip, stroke.write = (stroke.write+1) % MAX_STROKE;
                stroke.last = tip;
            }
        }
    } else {
        stroke.hasLast = false;
    }

    // simple vertical physics: eye height ~1.3 above the map's floor level
    float floorY = mapFloorY();
    cam.vy -= 18.0f * dt;
    cam.pos.y += cam.vy * dt;
    if(cam.pos.y <= floorY + 1.3f){
        cam.pos.y = floorY + 1.3f;
        cam.vy = 0.0f;
        cam.grounded = true;
    }
}

static void initWorld(void)
{
    handL.world.x = -0.35f; handL.world.y = 1.0f;  handL.world.z = 1.1f;
    handR.world.x =  0.35f; handR.world.y = 1.0f;  handR.world.z = 1.1f;
    handL.roll = 1.5708f;   // start idle: sideways, low
    handR.roll = -1.5708f;

    currentMap = 0;
    cam.pos.x = 0.0f; cam.pos.y = mapEyeY(); cam.pos.z = 0.0f;
    cam.yaw = 3.14159f; cam.pitch = 0.0f;
    cam.vy = 0.0f; cam.grounded = true;

    initBots();
    botMsg[0] = 0;
    botMsgTimer = 0.0f;

    // pen starts put away
    penHeld = false;
    penLift = 0.0f;
    stroke.count = 0;
    stroke.write = 0;
    stroke.hasLast = false;
}

// ---------------- menu (citro2d) ----------------

static void loadName(void)
{
    FILE* f = fopen(NAME_FILE, "r");
    if(f){
        if(fgets(playerName, NAME_MAX + 1, f)){
            int len = (int)strlen(playerName);
            while(len > 0 && (playerName[len-1]=='\n' || playerName[len-1]=='\r'))
                playerName[--len] = 0;
        }
        fclose(f);
    }
}

static void saveName(void)
{
    FILE* f = fopen(NAME_FILE, "w");
    if(f){
        fputs(playerName, f);
        fclose(f);
    }
}

static void sanitizeName(char* s)
{
    if(!s) return;
    size_t i, w = 0;
    for(i = 0; s[i]; i++){
        char c = s[i];
        if((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == ' '){
            s[w++] = c;
        }
    }
    s[w] = 0;
}

static void registerScreen(void)
{
    memset(playerName, 0, sizeof(playerName));

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, NAME_MAX);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "OK", true);
    swkbdSetHintText(&swkbd, "Enter your name");

    char out[NAME_MAX + 1];
    SwkbdButton ret = swkbdInputText(&swkbd, out, sizeof(out));
    if(ret == SWKBD_BUTTON_RIGHT){
        sanitizeName(out);
        if(out[0]){
            strncpy(playerName, out, NAME_MAX);
            playerName[NAME_MAX] = 0;
            saveName();
        }
    }
}

static void drawText(const char* s, float x, float y, float scale, u32 color, bool center)
{
    C2D_Text text;
    C2D_TextBufClear(g_textBuf);
    C2D_TextParse(&text, g_textBuf, s);
    C2D_TextOptimize(&text);
    if(center){
        float w, h;
        C2D_TextGetDimensions(&text, scale, scale, &w, &h);
        x -= w / 2.0f;
    }
    C2D_DrawText(&text, C2D_WithColor, x, y, 0.5f, scale, scale, color);
    C2D_TextBufClear(g_textBuf);
}

static void drawMenu(C3D_RenderTarget* top, C3D_RenderTarget* bottom)
{
    // top screen: the REC ROOM logo art fills the whole screen
    C2D_SceneBegin(top);
    C2D_TargetClear(top, clrOrange);
    drawMenuImage(0, SCREEN_W, SCREEN_H);

    // bottom screen: menu art with two white button strips; we draw the art
    // then put the PLAY / LOGIN ACCOUNT labels on the two strips.
    C2D_SceneBegin(bottom);
    C2D_TargetClear(bottom, clrOrange);
    drawMenuImage(1, 320.0f, 240.0f);

    // the two white strips were found at y 62..104 and y 152..194 on the
    // 320x240 art; highlight the selected one and print its label above it.
    float stripY[2] = { 62.0f, 152.0f };
    float stripH[2] = { 42.0f, 42.0f };
    int i;
    for(i = 0; i < MENU_ENTRIES; i++){
        if(menuSel == i){
            // subtle orange overlay to show which button is selected
            C2D_DrawRectSolid(20.0f, stripY[i] + 4.0f, 0.4f, 280.0f, stripH[i] - 8.0f,
                              C2D_Color32(0xFF, 0x6B, 0x0F, 0x40));
        }
    }
    drawText("PLAY", 160.0f, 80.0f, 1.2f, clrOrange, true);
    drawText("LOGIN ACCOUNT", 160.0f, 170.0f, 0.9f, clrOrange, true);
    drawText("Use D-Pad + A", 160.0f, 225.0f, 0.7f, clrWhite, true);
}

static void drawGameHud(C3D_RenderTarget* bottom)
{
    C2D_SceneBegin(bottom);
    C2D_TargetClear(bottom, clrOrange);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, clrOrangeD);

    drawText("@", 10.0f, 5.0f, 1.0f, clrWhite, false);
    drawText(playerName[0] ? playerName : "guest", 26.0f, 5.0f, 1.0f, clrWhite, false);

    drawText("STICK: walk", 160.0f, 32.0f, 1.0f, clrWhite, true);
    drawText("L/R + STICK: hands", 160.0f, 52.0f, 1.0f, clrWhite, true);
    drawText("X: look up   B: look down", 160.0f, 80.0f, 0.8f, clrWhite, true);
    drawText("Y: look left A: look right", 160.0f, 99.0f, 0.8f, clrWhite, true);
    drawText("L+R: grab   SELECT: talk", 160.0f, 125.0f, 1.0f, clrWhite, true);
    drawText(penHeld ? "R + STICK: draw" : "PEN: draw in air", 160.0f, 142.0f, 0.7f, clrWhite, true);
    // PLACES button (bottom screen, left; opens the place list)
    C2D_DrawCircleSolid(130.0f, 198.0f, 0.5f, 32.0f, placesOpen ? clrOrange : clrWhite);
    drawText("PLACES", 130.0f, 189.0f, 0.7f, placesOpen ? clrWhite : clrOrange, true);
    // PEN button (toggle) and JUMP button on the bottom screen
    C2D_DrawCircleSolid(200.0f, 198.0f, 0.5f, 32.0f, penHeld ? clrOrange : clrWhite);
    drawText(penHeld ? "PEN: ON" : "PEN", 200.0f, 189.0f, 0.8f, penHeld ? clrWhite : clrOrange, true);
    // JUMP button (bottom screen, where the right thumb naturally rests)
    C2D_DrawCircleSolid(270.0f, 198.0f, 0.5f, 32.0f, clrWhite);
    drawText("JUMP", 270.0f, 189.0f, 0.8f, clrOrange, true);
    {
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "v13 %s  yaw:%d pitch:%d",
                 PLACE_NAMES[currentMap],
                 (int)(cam.yaw*57.2958f), (int)(cam.pitch*57.2958f));
        drawText(dbg, 160.0f, 148.0f, 0.6f, clrWhite, true);
    }
    if(botMsgTimer > 0.0f && botMsg[0]){
        drawText(botMsg, 160.0f, 168.0f, 0.65f, clrWhite, true);
    }
    drawText("START: back to menu", 160.0f, 220.0f, 0.8f, clrWhite, true);

    // places menu overlay: list of all places, current one highlighted
    if(placesOpen){
        C2D_DrawRectSolid(20.0f, 40.0f, 0.5f, 280.0f, 30.0f, clrWhite);
        drawText("PLACES", 160.0f, 44.0f, 1.0f, clrOrange, true);
        int i;
        for(i = 0; i < PLACE_COUNT; i++){
            float y = 74.0f + i * 24.0f;
            if(i == currentMap)
                C2D_DrawRectSolid(30.0f, y, 0.5f, 260.0f, 22.0f, clrOrange);
            drawText(PLACE_NAMES[i], 160.0f, y + 8.0f, 0.8f,
                     i == currentMap ? clrWhite : clrWhite, true);
        }
        drawText("Tap a place to travel", 160.0f, 226.0f, 0.7f, clrWhite, true);
    }
}

static void gameLoop(C3D_RenderTarget* top, C3D_RenderTarget* bottom)
{
    initWorld();
    musicPlayGame(); // switch from menu track to the in-game track

    while(aptMainLoop()){
        hidScanInput();
        u32 kDown = hidKeysDown();

        if(kDown & KEY_START) break;
        if(kDown & KEY_SELECT) tryTalk();

        updatePlayer(1.0f/60.0f);
        updateHands();
        updateBots(1.0f/60.0f);
        updateCameraBasis();
        buildWorldFaces();

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        C2D_SceneBegin(top);
        C2D_TargetClear(top, C2D_Color32(0x20,0x20,0x20,0xFF));
        int i;
        for(i = 0; i < faceCount; i++)
            drawFaceQuad(&faces[i]);

        drawGameHud(bottom);
        C3D_FrameEnd(0);
    }

    musicStopAll();     // stop the in-game track when returning to the menu
    musicPlayMenu();    // menu music keeps playing on the menu screen
}

int main(void)
{
    gfxInitDefault();
    initColors();

    C3D_Init(0x800000); // 8 MB command buffer: ~5500 faces/frame (real room)
    C2D_Init(34000);    // vertex buffer: up to 5500 faces * 6 verts + HUD text
    C2D_Prepare();

    C3D_RenderTarget* top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    g_textBuf = C2D_TextBufNew(256);

    loadName();
    menuImagesInit();
    musicInit();
    musicPlayMenu();

    while(aptMainLoop()){
        hidScanInput();
        u32 kDown = hidKeysDown();

        if(kDown & KEY_START) break;

        if(kDown & KEY_DOWN){
            menuSel = (menuSel + 1) % MENU_ENTRIES;
        }
        if(kDown & KEY_UP){
            menuSel = (menuSel + MENU_ENTRIES - 1) % MENU_ENTRIES;
        }

        if(kDown & KEY_A){
            if(menuSel == 0){
                gameLoop(top, bottom);
            } else if(menuSel == 1){
                registerScreen();
            } else if(menuSel == 2){
                // nothing
            }
        }

        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
        drawMenu(top, bottom);
        C3D_FrameEnd(0);
    }

    C2D_TextBufDelete(g_textBuf);
    C2D_Fini();
    C3D_Fini();
    gfxExit();
    return 0;
}