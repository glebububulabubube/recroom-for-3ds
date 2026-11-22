#include <3ds.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include <citro2d.h>

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

#define MENU_ENTRIES 3
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
    int  grabIdx;   // box index it holds, -1 = empty
} Hand;

typedef struct {
    Vec3 pos;
    float yaw, pitch;
} Camera;

#define MAX_BOXES 8
static Box boxes[MAX_BOXES];
static Hand handL, handR;
static Camera cam;
static int grabbedBox = -1; // box currently attached to a hand

typedef struct {
    float depth;
    float x0,y0,x1,y1,x2,y2,x3,y3;
    u32 color;
} Face;

#define MAX_FACES 256
static Face faces[MAX_FACES];
static int faceCount = 0;

static int cmpFaces(const void* a, const void* b)
{
    float d = ((const Face*)b)->depth - ((const Face*)a)->depth;
    return d > 0.0f ? 1 : (d < 0.0f ? -1 : 0);
}

// camera-space projection; returns 0 if behind camera
static int project(float x, float y, float z, float* sx, float* sy, float* czOut)
{
    float rx = x - cam.pos.x, ry = y - cam.pos.y, rz = z - cam.pos.z;
    float cY = cosf(cam.yaw), sY = sinf(cam.yaw);
    float cP = cosf(cam.pitch), sP = sinf(cam.pitch);

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
}

static void drawFaceQuad(const Face* f)
{
    C2D_DrawTriangle(f->x0,f->y0,f->color,
                     f->x1,f->y1,f->color,
                     f->x2,f->y2,f->color, 0.5f);
    C2D_DrawTriangle(f->x0,f->y0,f->color,
                     f->x2,f->y2,f->color,
                     f->x3,f->y3,f->color, 0.5f);
}

static void addBoxFaces(const Box* b)
{
    float hx = b->size.x, hy = b->size.y, hz = b->size.z;
    float cx = b->pos.x, cy = b->pos.y, cz = b->pos.z;

    Vec3 corners[8] = {
        { cx-hx, cy-hy, cz-hz }, { cx+hx, cy-hy, cz-hz },
        { cx+hx, cy-hy, cz+hz }, { cx-hx, cy-hy, cz+hz },
        { cx-hx, cy+hy, cz-hz }, { cx+hx, cy+hy, cz-hz },
        { cx+hx, cy+hy, cz+hz }, { cx-hx, cy+hy, cz+hz }
    };

    Vec3 tmp[4];
    // bottom (dark)
    tmp[0]=corners[0]; tmp[1]=corners[1]; tmp[2]=corners[2]; tmp[3]=corners[3];
    addFace(tmp, b->color, 0.55f);
    // top (bright)
    tmp[0]=corners[4]; tmp[1]=corners[6]; tmp[2]=corners[5]; tmp[3]=corners[7];
    addFace(tmp, b->color, 1.0f);
    // back (z-)
    tmp[0]=corners[0]; tmp[1]=corners[4]; tmp[2]=corners[5]; tmp[3]=corners[1];
    addFace(tmp, b->color, 0.75f);
    // front (z+)
    tmp[0]=corners[3]; tmp[1]=corners[2]; tmp[2]=corners[6]; tmp[3]=corners[7];
    addFace(tmp, b->color, 0.85f);
    // left (x-)
    tmp[0]=corners[0]; tmp[1]=corners[3]; tmp[2]=corners[7]; tmp[3]=corners[4];
    addFace(tmp, b->color, 0.65f);
    // right (x+)
    tmp[0]=corners[1]; tmp[1]=corners[5]; tmp[2]=corners[6]; tmp[3]=corners[2];
    addFace(tmp, b->color, 0.7f);
}

static void buildWorldFaces(void)
{
    faceCount = 0;

    // floor
    {
        Vec3 f[4] = {
            {-30.0f, 0.0f, -30.0f}, { 30.0f, 0.0f, -30.0f},
            { 30.0f, 0.0f,  30.0f}, {-30.0f, 0.0f,  30.0f}
        };
        addFace(f, C2D_Color32(0xE8,0x5A,0x00,0xFF), 1.0f);
    }
    // walls
    {
        Vec3 w1[4] = {{-30,0,-30},{-30,0,30},{-30,5,30},{-30,5,-30}};
        addFace(w1, C2D_Color32(0xC8,0x48,0x00,0xFF), 1.0f);
        Vec3 w2[4] = {{30,0,-30},{30,0,30},{30,5,30},{30,5,-30}};
        addFace(w2, C2D_Color32(0xC8,0x48,0x00,0xFF), 0.8f);
        Vec3 w3[4] = {{-30,0,-30},{30,0,-30},{30,5,-30},{-30,5,-30}};
        addFace(w3, C2D_Color32(0xC8,0x48,0x00,0xFF), 0.9f);
        Vec3 w4[4] = {{-30,0,30},{30,0,30},{30,5,30},{-30,5,30}};
        addFace(w4, C2D_Color32(0xC8,0x48,0x00,0xFF), 0.95f);
    }

    int i;
    for(i = 0; i < MAX_BOXES; i++)
        if(!boxes[i].grabbed) addBoxFaces(&boxes[i]);

    // hands
    {
        Box h;
        h.color = C2D_Color32(0xFF,0xFF,0xFF,0xFF);
        h.pos = handL.world; h.size.x=0.12f; h.size.y=0.12f; h.size.z=0.12f;
        addBoxFaces(&h);
        h.pos = handR.world;
        addBoxFaces(&h);
    }

    qsort(faces, faceCount, sizeof(Face), cmpFaces);
}

static void moveHandTo(Hand* h, float tx, float ty, float tz)
{
    h->world.x += (tx - h->world.x) * 0.3f;
    h->world.y += (ty - h->world.y) * 0.3f;
    h->world.z += (tz - h->world.z) * 0.3f;
}

static void syncHandWithBox(Hand* h)
{
    if(h->grabIdx >= 0){
        Box* b = &boxes[h->grabIdx];
        b->pos = h->world;
        b->grabbed = true;
    }
}

static void updateHands(void)
{
    u32 held = hidKeysHeld();
    circlePosition pad;
    hidCircleRead(&pad);

    float dx = pad.dx / 156.0f, dy = pad.dy / 156.0f;
    if(dx > 1.0f) dx = 1.0f; if(dx < -1.0f) dx = -1.0f;
    if(dy > 1.0f) dy = 1.0f; if(dy < -1.0f) dy = -1.0f;
    if(fabsf(dx) < 0.15f) dx = 0.0f;
    if(fabsf(dy) < 0.15f) dy = 0.0f;

    float cY = cosf(cam.yaw), sY = sinf(cam.yaw);
    float cP = cosf(cam.pitch), sP = sinf(cam.pitch);
    float R[3] = { cY, 0.0f, -sY };
    float U[3] = { sY*sP, -cP, cY*sP };
    float F[3] = { sY*cP, sP, cY*cP };

    float px = cam.pos.x, py = cam.pos.y, pz = cam.pos.z;

    float txL, tyL, tzL, txR, tyR, tzR;

    if(held & KEY_L){
        // left hand follows stick (reaches forward)
        txL = px + R[0]*(dx*1.0f) + F[0]*1.4f + U[0]*(-dy*0.7f);
        tyL = py + R[1]*(dx*1.0f) + F[1]*1.4f + U[1]*(-dy*0.7f);
        tzL = pz + R[2]*(dx*1.0f) + F[2]*1.4f + U[2]*(-dy*0.7f);
    } else {
        // rest position: low left
        txL = px + R[0]*(-0.35f) + F[0]*1.1f + U[0]*0.45f;
        tyL = py + R[1]*(-0.35f) + F[1]*1.1f + U[1]*0.45f;
        tzL = pz + R[2]*(-0.35f) + F[2]*1.1f + U[2]*0.45f;
    }

    if(held & KEY_R){
        txR = px + R[0]*(dx*1.0f) + F[0]*1.4f + U[0]*(-dy*0.7f);
        tyR = py + R[1]*(dx*1.0f) + F[1]*1.4f + U[1]*(-dy*0.7f);
        tzR = pz + R[2]*(dx*1.0f) + F[2]*1.4f + U[2]*(-dy*0.7f);
    } else {
        // rest position: low right
        txR = px + R[0]*(0.35f) + F[0]*1.1f + U[0]*0.45f;
        tyR = py + R[1]*(0.35f) + F[1]*1.1f + U[1]*0.45f;
        tzR = pz + R[2]*(0.35f) + F[2]*1.1f + U[2]*0.45f;
    }

    moveHandTo(&handL, txL, tyL, tzL);
    moveHandTo(&handR, txR, tyR, tzR);
    syncHandWithBox(&handL);
    syncHandWithBox(&handR);
}

static void tryGrab(void)
{
    if(grabbedBox >= 0){
        boxes[grabbedBox].grabbed = false;
        boxes[grabbedBox].grabHand = -1;
        handL.grabIdx = -1;
        handR.grabIdx = -1;
        grabbedBox = -1;
        return;
    }

    // find nearest box to either hand, within reach
    float best = 1.4f * 1.4f;
    int bestIdx = -1;
    Vec3 bestHand;
    bool bestIsLeft = false;
    int i;
    for(i = 0; i < MAX_BOXES; i++){
        Box* b = &boxes[i];
        if(b->grabbed) continue;
        Vec3 dL = { b->pos.x - handL.world.x, b->pos.y - handL.world.y, b->pos.z - handL.world.z };
        Vec3 dR = { b->pos.x - handR.world.x, b->pos.y - handR.world.y, b->pos.z - handR.world.z };
        float distL = dL.x*dL.x + dL.y*dL.y + dL.z*dL.z;
        float distR = dR.x*dR.x + dR.y*dR.y + dR.z*dR.z;
        if(distL < best){ best = distL; bestIdx = i; bestHand = handL.world; bestIsLeft = true; }
        if(distR < best){ best = distR; bestIdx = i; bestHand = handR.world; bestIsLeft = false; }
    }
    if(bestIdx >= 0){
        grabbedBox = bestIdx;
        boxes[bestIdx].grabHand = bestIsLeft ? 0 : 1;
        boxes[bestIdx].grabbed = true;
        boxes[bestIdx].pos = bestIsLeft ? handL.world : handR.world;
        if(bestIsLeft) handL.grabIdx = bestIdx;
        else handR.grabIdx = bestIdx;
    }
}

static void updatePlayer(float dt)
{
    u32 held = hidKeysHeld();
    circlePosition pad;
    hidCircleRead(&pad);

    float dx = pad.dx / 156.0f, dy = pad.dy / 156.0f;
    if(dx > 1.0f) dx = 1.0f; if(dx < -1.0f) dx = -1.0f;
    if(dy > 1.0f) dy = 1.0f; if(dy < -1.0f) dy = -1.0f;
    if(fabsf(dx) < 0.15f) dx = 0.0f;
    if(fabsf(dy) < 0.15f) dy = 0.0f;

    // camera rotates while the button is held: X=left B=right Y=up A=down
    if(held & KEY_X) cam.yaw   -= 2.5f * dt;
    if(held & KEY_B) cam.yaw   += 2.5f * dt;
    if(held & KEY_Y) cam.pitch  = fminf(cam.pitch + 2.5f * dt,  1.4f);
    if(held & KEY_A) cam.pitch  = fmaxf(cam.pitch - 2.5f * dt, -1.4f);

    // walking only when hands are not moving (L/R not held)
    if(!(held & (KEY_L | KEY_R))){
        float cY = cosf(cam.yaw), sY = sinf(cam.yaw);
        float speed = 5.0f * dt;
        cam.pos.x += (cY*dx + sY*dy) * speed;
        cam.pos.z += (-sY*dx + cY*dy) * speed;
    }

    // clamp inside room
    if(cam.pos.x < -26.0f) cam.pos.x = -26.0f;
    if(cam.pos.x >  26.0f) cam.pos.x =  26.0f;
    if(cam.pos.z < -26.0f) cam.pos.z = -26.0f;
    if(cam.pos.z >  26.0f) cam.pos.z =  26.0f;
    cam.pos.y = 1.3f;
}

static void initWorld(void)
{
    // spread boxes around the room
    static const float coords[8][3] = {
        { -5, 0.4f, -5 }, { 5, 0.8f, -6 }, { -6, 0.3f, 4 }, { 4, 1.0f, 5 },
        { 0, 0.5f, 0 }, { -4, 0.7f, -1 }, { 6, 0.5f, 1 }, { -2, 0.6f, 7 }
    };
    int i;
    for(i = 0; i < MAX_BOXES; i++){
        boxes[i].pos.x = coords[i][0];
        boxes[i].pos.y = coords[i][1];
        boxes[i].pos.z = coords[i][2];
        boxes[i].size.x = 0.5f; boxes[i].size.y = 0.5f; boxes[i].size.z = 0.5f;
        switch(i){
            case 0: boxes[i].color = C2D_Color32(0xE8,0x4C,0x4C,0xFF); break;
            case 1: boxes[i].color = C2D_Color32(0x4C,0x8B,0xE8,0xFF); break;
            case 2: boxes[i].color = C2D_Color32(0x5C,0xD8,0x5C,0xFF); break;
            case 3: boxes[i].color = C2D_Color32(0xE8,0xD8,0x4C,0xFF); break;
            case 4: boxes[i].color = C2D_Color32(0xA8,0x5C,0xE8,0xFF); break;
            case 5: boxes[i].color = C2D_Color32(0x4C,0xE8,0xD8,0xFF); break;
            case 6: boxes[i].color = C2D_Color32(0xE8,0xA0,0x4C,0xFF); break;
            default: boxes[i].color = C2D_Color32(0xE8,0x7C,0xE8,0xFF); break;
        }
        boxes[i].grabbed = false;
        boxes[i].grabHand = -1;
    }

    handL.world.x = -0.35f; handL.world.y = 1.0f;  handL.world.z = 1.1f;
    handR.world.x =  0.35f; handR.world.y = 1.0f;  handR.world.z = 1.1f;
    handL.grabIdx = -1;
    handR.grabIdx = -1;
    grabbedBox = -1;

    cam.pos.x = 0.0f; cam.pos.y = 1.3f; cam.pos.z = 12.0f;
    cam.yaw = 0.0f; cam.pitch = 0.0f;
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
    C2D_SceneBegin(top);
    C2D_TargetClear(top, clrOrange);
    C2D_DrawRectSolid(0.0f, 60.0f, 0.0f, SCREEN_W, 150.0f, clrOrangeD);

    C2D_Text logo;
    C2D_TextBufClear(g_textBuf);
    C2D_TextParse(&logo, g_textBuf, "REC ROOM");
    C2D_TextOptimize(&logo);
    {
        float w, h;
        C2D_TextGetDimensions(&logo, 3.0f, 3.0f, &w, &h);
        C2D_DrawText(&logo, C2D_WithColor, (SCREEN_W - w) / 2.0f, 26.0f, 0.5f, 3.0f, 3.0f, clrWhite);
    }

    drawText("@", (SCREEN_W - 80.0f) / 2.0f, 100.0f, 1.5f, clrWhite, false);
    C2D_Text name;
    C2D_TextBufClear(g_textBuf);
    C2D_TextParse(&name, g_textBuf, playerName[0] ? playerName : "guest");
    C2D_TextOptimize(&name);
    {
        float w, h;
        C2D_TextGetDimensions(&name, 1.5f, 1.5f, &w, &h);
        C2D_DrawText(&name, C2D_WithColor, (SCREEN_W + 40.0f - w) / 2.0f, 100.0f, 0.5f, 1.5f, 1.5f, clrWhite);
    }
    C2D_TextBufClear(g_textBuf);

    float btnW = 250.0f, btnH = 36.0f;
    float btnX = (SCREEN_W - btnW) / 2.0f;
    if(menuSel == 0){
        C2D_DrawRectSolid(btnX, 140.0f, 0.3f, btnW, btnH, clrWhite);
        drawText("PLAY", SCREEN_W / 2.0f, 143.0f, 1.0f, clrOrange, true);
    } else {
        C2D_DrawRectSolid(btnX, 140.0f, 0.3f, btnW, btnH, clrOrangeD);
        drawText("PLAY", SCREEN_W / 2.0f, 143.0f, 1.0f, clrWhite, true);
    }

    if(menuSel == 1){
        C2D_DrawRectSolid(btnX, 185.0f, 0.3f, btnW, btnH, clrOrangeD);
        drawText("SWITCH ACCOUNT", SCREEN_W / 2.0f, 188.0f, 0.8f, clrWhite, true);
    } else {
        C2D_DrawRectSolid(btnX, 185.0f, 0.3f, btnW, btnH, clrOrange);
        drawText("SWITCH ACCOUNT", SCREEN_W / 2.0f, 188.0f, 0.8f, clrWhite, true);
    }

    if(menuSel == 2){
        drawText("Language", SCREEN_W / 2.0f, 225.0f, 0.8f, clrWhite, true);
    } else {
        drawText("Language", SCREEN_W / 2.0f, 225.0f, 0.8f, clrGrey, true);
    }

    C2D_SceneBegin(bottom);
    C2D_TargetClear(bottom, clrOrange);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, clrOrangeD);
    drawText("Use D-Pad to select", 160.0f, 10.0f, 0.8f, clrWhite, true);
    drawText("A - confirm", 160.0f, 110.0f, 0.8f, clrWhite, true);
    drawText("START - exit", 160.0f, 210.0f, 0.8f, clrWhite, true);
}

static void drawGameHud(C3D_RenderTarget* bottom)
{
    C2D_SceneBegin(bottom);
    C2D_TargetClear(bottom, clrOrange);
    C2D_DrawRectSolid(0.0f, 0.0f, 0.0f, 320.0f, 240.0f, clrOrangeD);

    drawText("@", 10.0f, 5.0f, 1.0f, clrWhite, false);
    drawText(playerName[0] ? playerName : "guest", 26.0f, 5.0f, 1.0f, clrWhite, false);

    drawText("STICK: walk", 160.0f, 40.0f, 1.0f, clrWhite, true);
    drawText("L/R + STICK: hands", 160.0f, 65.0f, 1.0f, clrWhite, true);
    drawText("X: look left   B: look right", 160.0f, 100.0f, 0.8f, clrWhite, true);
    drawText("Y: look up     A: look down", 160.0f, 125.0f, 0.8f, clrWhite, true);
    drawText("L+R: grab/release", 160.0f, 155.0f, 1.0f, clrWhite, true);
    {
        char dbg[64];
        snprintf(dbg, sizeof(dbg), "v2  yaw:%d pitch:%d", (int)(cam.yaw*57.2958f), (int)(cam.pitch*57.2958f));
        drawText(dbg, 160.0f, 190.0f, 0.6f, clrWhite, true);
    }
    drawText("START: back to menu", 160.0f, 220.0f, 0.8f, clrWhite, true);
}

static void gameLoop(C3D_RenderTarget* top, C3D_RenderTarget* bottom)
{
    initWorld();

    while(aptMainLoop()){
        hidScanInput();
        u32 kDown = hidKeysDown();

        if(kDown & KEY_START) break;
        if((kDown & KEY_L) && (kDown & KEY_R)) tryGrab();

        updatePlayer(1.0f/60.0f);
        updateHands();
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
}

int main(void)
{
    gfxInitDefault();
    initColors();

    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
    C2D_Prepare();

    C3D_RenderTarget* top    = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);
    C3D_RenderTarget* bottom = C2D_CreateScreenTarget(GFX_BOTTOM, GFX_LEFT);

    g_textBuf = C2D_TextBufNew(256);

    loadName();

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
