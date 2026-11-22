#include <3ds.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define NAME_MAX 16
#define NAME_FILE "sdmc:/recroom3ds_name.txt"

static char playerName[NAME_MAX + 1] = "";
static int nameLen = 0;

static void loadName(void){
    FILE* f = fopen(NAME_FILE, "r");
    if(f){
        if(fgets(playerName, NAME_MAX + 1, f)){
            nameLen = (int)strlen(playerName);
            while(nameLen > 0 && (playerName[nameLen-1]=='\n' || playerName[nameLen-1]=='\r'))
                playerName[--nameLen] = 0;
        }
        fclose(f);
    }
}

static void saveName(void){
    FILE* f = fopen(NAME_FILE, "w");
    if(f){
        fputs(playerName, f);
        fclose(f);
    }
}

// ---- registration screen: type name with on-screen keyboard ----
static void registerScreen(void){
    memset(playerName, 0, sizeof(playerName));
    nameLen = 0;

    SwkbdState swkbd;
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, NAME_MAX);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_LEFT, "Cancel", false);
    swkbdSetButton(&swkbd, SWKBD_BUTTON_RIGHT, "OK", true);
    swkbdSetHintText(&swkbd, "Введите имя");

    char out[NAME_MAX + 1];
    SwkbdButton ret = swkbdInputText(&swkbd, out, sizeof(out));
    if(ret == SWKBD_BUTTON_RIGHT){
        strncpy(playerName, out, NAME_MAX);
        playerName[NAME_MAX] = 0;
        nameLen = (int)strlen(playerName);
        saveName();
    }
}

// ---- main menu ----
enum { MENU_PLAY = 0, MENU_NAME, MENU_EXIT, MENU_COUNT };
static int menuSel = 0;

static void drawMenu(void){
    printf("\x1b[2J\x1b[H");
    printf("=== RecRoom3DS ===\n\n");
    printf("Игрок: %s\n\n", playerName[0] ? playerName : "(не зарегистрирован)");
    printf("%s Играть\n", menuSel==MENU_PLAY ? ">" : " ");
    printf("%s Изменить имя\n", menuSel==MENU_NAME ? ">" : " ");
    printf("%s Выход\n", menuSel==MENU_EXIT ? ">" : " ");
    printf("\nA - выбрать, START - выход\n");
}

static void drawPlay(void){
    printf("\x1b[2J\x1b[H");
    printf("=== Игра ===\n\n");
    printf("Привет, %s!\n", playerName[0] ? playerName : "игрок");
    printf("\n3D-мир скоро появится здесь.\n");
    printf("\nB - назад в меню\n");
}

int main(void){
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);

    loadName();

    while(aptMainLoop()){
        hidScanInput();
        u32 kDown = hidKeysDown();

        if(kDown & KEY_START) break;

        if(kDown & KEY_DOWN){
            menuSel = (menuSel + 1) % MENU_COUNT;
            drawMenu();
        }
        if(kDown & KEY_UP){
            menuSel = (menuSel + MENU_COUNT - 1) % MENU_COUNT;
            drawMenu();
        }

        if(kDown & KEY_A){
            if(menuSel == MENU_PLAY){
                drawPlay();
                while(aptMainLoop()){
                    hidScanInput();
                    if(hidKeysDown() & KEY_B) break;
                }
                drawMenu();
            } else if(menuSel == MENU_NAME){
                registerScreen();
                drawMenu();
            } else if(menuSel == MENU_EXIT){
                break;
            }
        }
    }

    gfxExit();
    return 0;
}
