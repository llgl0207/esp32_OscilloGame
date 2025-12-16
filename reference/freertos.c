/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2025 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "fatfs.h"
#include "draw.h"
#include "tim.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
typedef enum {
    UI_MENU_MAIN,
    UI_MENU_MUSIC_LIST,
    UI_PLAYING,
    UI_MENU_TEXT_LIST,
    UI_TEXT_VIEWER,
    UI_MENU_GAME_LIST,
    UI_GAME,
    UI_BREAKOUT,
    UI_FLAPPY,
    UI_RACING,
    UI_RUNTINY,
    UI_DEBUG_INPUT,
    UI_SETTINGS,
    UI_SETTINGS_DRAW_MODE,
    UI_SETTINGS_CPU_SPEED,
    UI_SETTINGS_CPU_JUMP,
    UI_SETTINGS_DRAW_DENSITY
} UI_State;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define SD_WAVE_MAX_LEN 16384

// --- Breakout Game Defines ---
#define BRK_PADDLE_W 800
#define BRK_PADDLE_H 100
#define BRK_BALL_R 60
#define BRK_ROWS 5
#define BRK_COLS 8
#define BRK_BRICK_W (4096 / BRK_COLS)
#define BRK_BRICK_H 250

// --- Flappy Game Defines ---
#define FLP_GRAVITY 4
#define FLP_JUMP_FORCE 70
#define FLP_SPEED 30
#define FLP_GAP_H 1100
#define FLP_OBSTACLE_W 300
#define FLP_OBSTACLE_SPACING 1600
#define FLP_PLAYER_X 1000
#define FLP_PLAYER_R 80

// --- Racing Game Defines ---
#define RACE_CAR_W 300
#define RACE_CAR_H 500
#define RACE_OBSTACLE_W 300
#define RACE_OBSTACLE_H 300
#define RACE_SPEED 40
#define RACE_MAX_OBSTACLES 5

// --- RunTiny Game Defines ---
#define RUN_GROUND_Y 500
#define RUN_PLAYER_X 800
#define RUN_PLAYER_W 200
#define RUN_PLAYER_H 300
#define RUN_JUMP_FORCE 60
#define RUN_GRAVITY 4
#define RUN_SPEED 40
#define RUN_OBSTACLE_W 150
#define RUN_OBSTACLE_H 200
#define RUN_MAX_OBSTACLES 3
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
extern FIL fil;
extern FATFS fs;
extern SD_HandleTypeDef hsd;
extern uint8_t *SD_Wave_Buffer; // Changed to pointer
extern volatile uint32_t SD_Wave_Idx;
extern volatile uint32_t SD_Wave_Write_Idx;
extern uint8_t SD_Wave_Loaded;
extern uint32_t SD_Wave_Total_Data_Left;
extern uint8_t Video_Mode;
volatile uint8_t Playback_Finished = 0;

extern uint16_t volume;
extern uint8_t pitch;
extern int scale;
extern uint8_t func;
extern uint8_t Music_Score[];
extern uint8_t Wave[];
extern volatile int8_t Game_Input_Dir;

// Music Player Globals
#define MAX_MUSIC_FILES 20
#define MAX_FILENAME_LEN 32
char music_files[MAX_MUSIC_FILES][MAX_FILENAME_LEN];
int music_file_count = 0;
char current_playing_file[MAX_FILENAME_LEN];

// --- Text Viewer Variables ---
#define MAX_TEXT_LINES 1000
char *text_lines[MAX_TEXT_LINES];
int total_text_lines = 0;
int current_text_line = 0;

// --- Snake Game Variables ---
#define SNAKE_GRID_SIZE 20
#define SNAKE_MAX_LEN 100
typedef struct {
    int8_t x;
    int8_t y;
} Point;
Point snake_body[SNAKE_MAX_LEN];
int snake_len = 3;
Point snake_food;
int snake_dir = 3; // 0:UP, 1:DOWN, 2:LEFT, 3:RIGHT
int game_over = 0;
uint32_t last_game_tick = 0;
int game_score = 0;

// --- Breakout Game Variables ---
typedef struct {
    int32_t x, y;
    int32_t vx, vy;
} BrkBall;

typedef struct {
    int32_t x;
} BrkPaddle;

uint8_t brk_bricks[BRK_ROWS][BRK_COLS];
BrkBall brk_ball;
BrkPaddle brk_paddle;
int brk_score = 0;
int brk_lives = 3;
int brk_game_over = 0;
int brk_brick_count = 0;

// --- Flappy Game Variables ---
typedef struct {
    int32_t y;
    int32_t vy;
} FlpPlayer;

typedef struct {
    int32_t x;
    int32_t gap_y; // Center of gap
    int active;
    int passed;
} FlpObstacle;

#define FLP_MAX_OBSTACLES 3
FlpPlayer flp_player;
FlpObstacle flp_obstacles[FLP_MAX_OBSTACLES];
int flp_score = 0;
int flp_game_over = 0;

// --- Racing Game Variables ---
typedef struct {
    int32_t x;
} RaceCar;

typedef struct {
    int32_t x, y;
    int active;
    int passed;
} RaceObstacle;

RaceCar race_car;
RaceObstacle race_obstacles[RACE_MAX_OBSTACLES];
int race_score = 0;
int race_game_over = 0;

// --- RunTiny Game Variables ---
typedef struct {
    int32_t y;
    int32_t vy;
    int jumping;
} RunPlayer;

typedef struct {
    int32_t x;
    int active;
    int passed;
} RunObstacle;

RunPlayer run_player;
RunObstacle run_obstacles[RUN_MAX_OBSTACLES];
int run_score = 0;
int run_game_over = 0;

volatile int SD_Status = 0; // 0: Waiting, 1: OK, <0: Error Code

/* USER CODE END Variables */
osThreadId defaultTaskHandle;
osThreadId myTask02Handle;
osThreadId myTask03Handle;

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void Scan_Music_Files(const char* path);
void Play_Music(char* filename);
void Play_Video(char* filename);
void Stop_Music(void);
void Scan_Text_Files(const char* path);
void Open_Text_File(char* filename);
void Init_Snake_Game(void);
void Update_Snake_Game(void);
void Init_Breakout_Game(void);
void Update_Breakout_Game(int16_t encoder_delta);
void Init_Flappy_Game(void);
void Update_Flappy_Game(int jump_requested);
void Init_Racing_Game(void);
void Update_Racing_Game(int16_t encoder_delta);
void Init_RunTiny_Game(void);
void Update_RunTiny_Game(int jump_requested);

void Show_Boot_Sequence(void);
void Show_Loading_Sequence(const char* game_name);

// --- Scroll State Management ---
#define MAX_SCROLL_STATES 16
typedef struct {
    char text[64];
    int32_t offset;
} ScrollState;

static ScrollState saved_scroll_states[MAX_SCROLL_STATES];
static int saved_scroll_count = 0;

static void SaveScroll(const char* txt) {
    if(saved_scroll_count >= MAX_SCROLL_STATES) return;
    int32_t off = DRAW_GetTextScroll(txt);
    if(off != 0) {
        strncpy(saved_scroll_states[saved_scroll_count].text, txt, 63);
        saved_scroll_states[saved_scroll_count].text[63] = '\0';
        saved_scroll_states[saved_scroll_count].offset = off;
        saved_scroll_count++;
    }
}

static void RestoreScroll(int16_t slot, const char* txt) {
    for(int i=0; i<saved_scroll_count; i++) {
        if(strncmp(saved_scroll_states[i].text, txt, 63) == 0) {
            DRAW_SetTextScroll(slot, saved_scroll_states[i].offset);
            return;
        }
    }
}

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void const * argument);
void AudioTask(void const * argument);
void GuiTask(void const * argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* USER CODE BEGIN GET_IDLE_TASK_MEMORY */
static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize )
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
  /* place for user code */
}
/* USER CODE END GET_IDLE_TASK_MEMORY */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityNormal, 0, 1024);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  /* definition and creation of myTask02 */
  osThreadDef(myTask02, AudioTask, osPriorityHigh, 0, 1024);
  myTask02Handle = osThreadCreate(osThread(myTask02), NULL);

  /* definition and creation of myTask03 */
  osThreadDef(myTask03, GuiTask, osPriorityNormal, 0, 1024);
  myTask03Handle = osThreadCreate(osThread(myTask03), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
  * @brief  Function implementing the defaultTask thread.
  * @param  argument: Not used
  * @retval None
  */
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void const * argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  
  // --- SD Card Initialization ---
  osDelay(1000); // Wait for power stabilization
  
  FRESULT res;
  
  // Mount SD Card
  res = f_mount(&fs, "0:", 1);
  if(res == FR_OK)
  {
      SD_Status = 1;
      
      /* 
      // Auto-play test.wav DISABLED to prevent interference with Menu
      // Open WAV file
      res = f_open(&fil, "test.wav", FA_READ);
      if(res == FR_OK)
      {
          UINT br;
          uint8_t header[44];
          
          // Read WAV Header
          f_read(&fil, header, 44, &br);
          
          // Simple WAV Validation (RIFF, WAVE)
          if(strncmp((char*)header, "RIFF", 4) == 0 && strncmp((char*)&header[8], "WAVE", 4) == 0)
          {
              // Parse Data Size (Little Endian at offset 40)
              uint32_t data_size = header[40] | (header[41] << 8) | (header[42] << 16) | (header[43] << 24);
              SD_Wave_Total_Data_Left = data_size;
              
              // Pre-fill Buffer
              uint32_t to_read = (data_size > SD_WAVE_MAX_LEN) ? SD_WAVE_MAX_LEN : data_size;
              
              f_read(&fil, SD_Wave_Buffer, to_read, &br);
              
              SD_Wave_Idx = 0;
              SD_Wave_Write_Idx = (br < SD_WAVE_MAX_LEN) ? br : 0;
              SD_Wave_Total_Data_Left -= br;
              
              SD_Wave_Loaded = 1;
              
              char msg[32];
              sprintf(msg, "PLAYING: %d KB\n", data_size/1024);
              DRAW_Terminal_Print(msg);
              
              // Adjust Timer Frequency for 44.1kHz
              __HAL_TIM_SET_PRESCALER(&htim3, 0);
              __HAL_TIM_SET_AUTORELOAD(&htim3, 1904);
              __HAL_TIM_SET_COUNTER(&htim3, 0);
              DRAW_Terminal_Print("FREQ SET: 44.1kHz\n");
          }
          else
          {
              DRAW_Terminal_Print("INVALID WAV\n");
              f_close(&fil);
          }
      }
      else
      {
          DRAW_Terminal_Print("NO test.wav\n");
      }
      */
  }
  else
  {
      SD_Status = -res;
  }

  /* Infinite loop */
  for(;;)
  {
    osDelay(1000);
  }
  /* USER CODE END StartDefaultTask */
}

/* USER CODE BEGIN Header_AudioTask */
/**
* @brief Function implementing the myTask02 thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_AudioTask */
void AudioTask(void const * argument)
{
  /* USER CODE BEGIN AudioTask */
  /* Infinite loop */
  for(;;)
  {
    if(SD_Wave_Loaded)
    {
        // Streaming Logic
        uint32_t buffered;
        uint32_t current_read_idx = SD_Wave_Idx; // Snapshot volatile variable
        
        if(SD_Wave_Write_Idx >= current_read_idx)
            buffered = SD_Wave_Write_Idx - current_read_idx;
        else
            buffered = SD_WAVE_MAX_LEN - (current_read_idx - SD_Wave_Write_Idx);
        
        // If buffer is less than half full, refill
        if(buffered < (SD_WAVE_MAX_LEN / 2))
        {
            UINT br;
            uint32_t to_read = 4096; // Read 4KB chunks
            FRESULT res;
            
            // Determine where to write
            uint32_t space_at_end = SD_WAVE_MAX_LEN - SD_Wave_Write_Idx;
            
            if(to_read > space_at_end)
            {
                // Split read
                res = f_read(&fil, &SD_Wave_Buffer[SD_Wave_Write_Idx], space_at_end, &br);
                
                if(br < space_at_end) { 
                    Playback_Finished = 1;
                    SD_Wave_Loaded = 0;
                    continue;
                } 
                
                res = f_read(&fil, &SD_Wave_Buffer[0], to_read - space_at_end, &br);
                
                if(br < (to_read - space_at_end)) { 
                    Playback_Finished = 1;
                    SD_Wave_Loaded = 0;
                    continue;
                }
                
                SD_Wave_Write_Idx = to_read - space_at_end;
            }
            else
            {
                res = f_read(&fil, &SD_Wave_Buffer[SD_Wave_Write_Idx], to_read, &br);
                
                if(br < to_read) { 
                    Playback_Finished = 1;
                    SD_Wave_Loaded = 0;
                    continue;
                } 
                
                SD_Wave_Write_Idx += to_read;
                if(SD_Wave_Write_Idx >= SD_WAVE_MAX_LEN) SD_Wave_Write_Idx = 0;
            }
        }
        osDelay(2); // Check every 2ms
    }
    else
    {
        osDelay(100);
    }
  }
  /* USER CODE END AudioTask */
}

/* USER CODE BEGIN Header_GuiTask */
/**
* @brief Function implementing the myTask03 thread.
* @param argument: Not used
* @retval None
*/
/* USER CODE END Header_GuiTask */
void GuiTask(void const * argument)
{
  /* USER CODE BEGIN GuiTask */
  
  Show_Boot_Sequence();

  UI_State ui_state = UI_MENU_MAIN;
  
  // Main Menu Items
  const char *main_menu_items[] = {
      "MUSIC PLAYER",
      "VIDEO PLAYER",
      "TEXT BROWSER",
      "DEBUG INPUT",
      "GAME",
      "SETTINGS",   
      "ABOUT",
      "EXIT"
  };
  const int main_menu_count = 8;

  // Game Menu Items
  const char *game_menu_items[] = {
      "BACK",
      "SNAKE",
      "BREAK",
      "FLAPPY",
      "RACING",
      "RUNTINY"
  };
  const int game_menu_count = 6;
  
  // Settings Menu Items
  const char *settings_menu_items[] = {
      "BACK",
      "DRAW MODE",
      "CPU SPEED",
      "JUMP DWELL",
      "DENSITY"
  };
  const int settings_menu_count = 5;
  static int current_draw_mode_idx = 0; // 0:DMA, 1:CPU

  // Draw Mode Menu Items
  const char *draw_mode_menu_items[] = {
      "BACK",
      "DMA",
      "CPU"
  };
  const int draw_mode_menu_count = 3;
  
  // State Variables
  int menu_index = 0;
  int menu_scroll = 0;
  int last_menu_index = -1;
  int last_menu_scroll = -1;
  int menu_mode = 0; // 0: Music, 1: Video, 2: Text
  
  // UI Constants
  const int line_height = 700;
  const int start_y = 3500;
  const int visible_lines = 5;
  
  DRAW_Clear();
  
  static uint16_t last_encoder = 0;
  static int32_t encoder_acc = 0;
  // static uint32_t last_blink_time = 0;
  static int blink_state = 1;
  
  /* Infinite loop */
  for(;;)
  {
    // Update Drawing Animation
    if(!Video_Mode) {
        DRAW_Update();
    }

    HAL_GPIO_TogglePin(LED_GPIO_Port, LED_Pin);
    
    // Encoder Logic
    uint16_t encoder = __HAL_TIM_GET_COUNTER(&htim8);
    int16_t delta = (int16_t)(encoder - last_encoder);
    last_encoder = encoder;

    if(ui_state == UI_GAME) {
        Update_Snake_Game();
    } else if(ui_state == UI_BREAKOUT) {
        Update_Breakout_Game(delta);
    } else if(ui_state == UI_FLAPPY) {
        Update_Flappy_Game(0); // Normal update, jump handled in button section
    } else if(ui_state == UI_RACING) {
        Update_Racing_Game(delta);
    } else if(ui_state == UI_RUNTINY) {
        Update_RunTiny_Game(0);
    } else if(ui_state == UI_SETTINGS_CPU_SPEED) {
        if(delta != 0) {
            int32_t d = (int32_t)DRAW_GetCPUDelay() + delta;
            if(d < 0) d = 0;
            if(d > 1000) d = 1000;
            DRAW_SetCPUDelay((uint32_t)d);
        }
    } else if(ui_state == UI_SETTINGS_CPU_JUMP) {
        if(delta != 0) {
            int32_t d = (int32_t)DRAW_GetCPUJumpDwell() + delta;
            if(d < 0) d = 0;
            if(d > 1000) d = 1000;
            DRAW_SetCPUJumpDwell((uint32_t)d);
        }
    } else if(ui_state == UI_SETTINGS_DRAW_DENSITY) {
        if(delta != 0) {
            int32_t d = (int32_t)DRAW_GetDrawDensity() + (delta * 10);
            if(d < 10) d = 10;
            if(d > 1000) d = 1000;
            DRAW_SetDrawDensity((uint32_t)d);
        }
    }
    
    // Handle Volume in Playing State
    if(ui_state == UI_PLAYING) {
        if(delta != 0) {
             int32_t new_vol = (int32_t)volume + (delta * 2);
             if(new_vol > 2000) new_vol = 2000;
             if(new_vol < 0) new_vol = 0;
             volume = (uint16_t)new_vol;
        }
        
        if(Playback_Finished) {
             Stop_Music();
             if(strncmp(current_playing_file, "welcome.wav", MAX_FILENAME_LEN) == 0) {
                 ui_state = UI_MENU_MAIN;
                 menu_index = 6; // Return to About
                 menu_mode = 0; // Reset mode
             } else {
                 ui_state = UI_MENU_MUSIC_LIST;
             }
             last_menu_index = -1;
             Playback_Finished = 0;
        }
    } else if(ui_state == UI_TEXT_VIEWER) {
        // Text Scrolling
        encoder_acc += delta;
        if(abs(encoder_acc) >= 4) {
            int steps = encoder_acc / 4;
            current_text_line += steps;
            encoder_acc -= steps * 4;
            
            if(current_text_line < 0) current_text_line = 0;
            if(current_text_line > total_text_lines - 5) current_text_line = total_text_lines - 5;
            if(current_text_line < 0) current_text_line = 0;
        }
    } else if(ui_state == UI_SETTINGS_CPU_SPEED) {
        // Do nothing here, handled above
    } else if(ui_state == UI_SETTINGS_CPU_JUMP) {
        // Do nothing here, handled above
    } else if(ui_state == UI_SETTINGS_DRAW_DENSITY) {
        // Do nothing here, handled above
    } else {
        // Menu Navigation
        encoder_acc += delta;
        if(abs(encoder_acc) >= 4) {
            int steps = encoder_acc / 4;
            menu_index += steps;
            encoder_acc -= steps * 4;
            
            int max_items = 0;
            if(ui_state == UI_MENU_MAIN) max_items = main_menu_count;
            else if(ui_state == UI_MENU_MUSIC_LIST || ui_state == UI_MENU_TEXT_LIST) max_items = music_file_count;
            else if(ui_state == UI_MENU_GAME_LIST) max_items = game_menu_count;
            else if(ui_state == UI_SETTINGS) max_items = settings_menu_count;
            else if(ui_state == UI_SETTINGS_DRAW_MODE) max_items = draw_mode_menu_count;
            
            if(menu_index < 0) menu_index = 0;
            if(menu_index >= max_items) menu_index = max_items - 1;
        }
    }
    
    // --- Flappy Bird Specific Input (Non-blocking) ---
    if(ui_state == UI_FLAPPY && !flp_game_over) {
        static uint8_t last_up_state = 1; // Assumes Pull-Up (1=Released)
        uint8_t current_up_state = HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin);
        
        if(current_up_state == 0 && last_up_state == 1) { // Falling Edge (Press)
            Update_Flappy_Game(1); // Jump
        }
        last_up_state = current_up_state;
    }

    // --- RunTiny Specific Input (Non-blocking) ---
    if(ui_state == UI_RUNTINY && !run_game_over) {
        static uint8_t last_up_state = 1;
        uint8_t current_up_state = HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin);
        
        if(current_up_state == 0 && last_up_state == 1) { // Falling Edge (Press)
            Update_RunTiny_Game(1); // Jump
        }
        last_up_state = current_up_state;
    }

    // Button Logic (RS_Pin)
    if(HAL_GPIO_ReadPin(RS_GPIO_Port, RS_Pin) == GPIO_PIN_RESET) {
        osDelay(20); // Debounce
        if(HAL_GPIO_ReadPin(RS_GPIO_Port, RS_Pin) == GPIO_PIN_RESET) {
             
             if(ui_state == UI_FLAPPY) {
                 // Exit Game
                 ui_state = UI_MENU_GAME_LIST;
                 menu_index = 0;
                 last_menu_index = -1;
                 while(HAL_GPIO_ReadPin(RS_GPIO_Port, RS_Pin) == GPIO_PIN_RESET) osDelay(10);
             } else if(ui_state == UI_RACING) {
                 // Exit Game
                 ui_state = UI_MENU_GAME_LIST;
                 menu_index = 0;
                 last_menu_index = -1;
                 while(HAL_GPIO_ReadPin(RS_GPIO_Port, RS_Pin) == GPIO_PIN_RESET) osDelay(10);
             } else if(ui_state == UI_RUNTINY) {
                 // Exit Game
                 ui_state = UI_MENU_GAME_LIST;
                 menu_index = 0;
                 last_menu_index = -1;
                 while(HAL_GPIO_ReadPin(RS_GPIO_Port, RS_Pin) == GPIO_PIN_RESET) osDelay(10);
             }
             else {
                 while(HAL_GPIO_ReadPin(RS_GPIO_Port, RS_Pin) == GPIO_PIN_RESET) osDelay(10);
                 
                 if(ui_state == UI_MENU_MAIN) {
                     if(menu_index == 0) { // Music Player
                         Scan_Music_Files("/music");
                         ui_state = UI_MENU_MUSIC_LIST;
                         menu_mode = 0;
                         menu_index = 0;
                         menu_scroll = 0;
                         last_menu_index = -1; 
                     } else if(menu_index == 1) { // Video Player
                         Scan_Music_Files("/video");
                         ui_state = UI_MENU_MUSIC_LIST;
                         menu_mode = 1;
                         menu_index = 0;
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else if(menu_index == 2) { // Text Browser
                         Scan_Text_Files("/text");
                         ui_state = UI_MENU_TEXT_LIST;
                         menu_mode = 2;
                         menu_index = 0;
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else if(menu_index == 3) { // Debug Input
                         ui_state = UI_DEBUG_INPUT;
                         last_menu_index = -1;
                     } else if(menu_index == 4) { // Game Menu
                         ui_state = UI_MENU_GAME_LIST;
                         menu_index = 0;
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else if(menu_index == 5) { // Settings
                         ui_state = UI_SETTINGS;
                         menu_index = 0;
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else if(menu_index == 6) { // About
                         strncpy(current_playing_file, "welcome.wav", MAX_FILENAME_LEN);
                         Play_Video("/system/welcome.wav");
                         ui_state = UI_PLAYING;
                         menu_mode = 1; // Video Mode
                         last_menu_index = -1;
                     }
                 } else if(ui_state == UI_SETTINGS) {
                     if(menu_index == 0) { // Back
                         ui_state = UI_MENU_MAIN;
                         menu_index = 5;
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else if(menu_index == 1) { // Draw Mode
                         ui_state = UI_SETTINGS_DRAW_MODE;
                         menu_index = 0;
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else if(menu_index == 2) { // CPU Speed
                         ui_state = UI_SETTINGS_CPU_SPEED;
                         last_menu_index = -1;
                     } else if(menu_index == 3) { // Jump Dwell
                         ui_state = UI_SETTINGS_CPU_JUMP;
                         last_menu_index = -1;
                     } else if(menu_index == 4) { // Draw Density
                         ui_state = UI_SETTINGS_DRAW_DENSITY;
                         last_menu_index = -1;
                     }
                 } else if(ui_state == UI_SETTINGS_DRAW_MODE) {
                     if(menu_index == 0) { // Back
                         ui_state = UI_SETTINGS;
                         menu_index = 1;
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else if(menu_index == 1) { // DMA
                         current_draw_mode_idx = 0;
                         DRAW_SetMode(DRAW_MODE_DMA);
                         last_menu_index = -1;
                     } else if(menu_index == 2) { // CPU
                         current_draw_mode_idx = 1;
                         DRAW_SetMode(DRAW_MODE_CPU);
                         last_menu_index = -1;
                     }
                 } else if(ui_state == UI_SETTINGS_CPU_SPEED) {
                     ui_state = UI_SETTINGS;
                     menu_index = 2;
                     menu_scroll = 0;
                     last_menu_index = -1;
                 } else if(ui_state == UI_SETTINGS_CPU_JUMP) {
                     ui_state = UI_SETTINGS;
                     menu_index = 3;
                     menu_scroll = 0;
                     last_menu_index = -1;
                 } else if(ui_state == UI_SETTINGS_DRAW_DENSITY) {
                     ui_state = UI_SETTINGS;
                     menu_index = 4;
                     menu_scroll = 0;
                     last_menu_index = -1;
                 } else if(ui_state == UI_MENU_MUSIC_LIST) {
                     if(menu_index == 0) { // Back
                         ui_state = UI_MENU_MAIN;
                         menu_index = 0;
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else {
                         // Play File
                         strncpy(current_playing_file, music_files[menu_index], MAX_FILENAME_LEN);
                         if(menu_mode == 0) Play_Music(current_playing_file);
                         else Play_Video(current_playing_file);
                         ui_state = UI_PLAYING;
                         last_menu_index = -1;
                     }
                 } else if(ui_state == UI_MENU_TEXT_LIST) {
                     if(menu_index == 0) { // Back
                         ui_state = UI_MENU_MAIN;
                         menu_index = 0;
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else {
                         // Open Text File
                         Open_Text_File(music_files[menu_index]);
                         ui_state = UI_TEXT_VIEWER;
                         last_menu_index = -1; // Force redraw
                     }
                 } else if(ui_state == UI_MENU_GAME_LIST) {
                     if(menu_index == 0) { // Back
                         ui_state = UI_MENU_MAIN;
                         menu_index = 4; // Return to Game option
                         menu_scroll = 0;
                         last_menu_index = -1;
                     } else if(menu_index == 1) { // Snake
                         Show_Loading_Sequence("SNAKE");
                         Init_Snake_Game();
                         ui_state = UI_GAME;
                         last_menu_index = -1;
                     } else if(menu_index == 2) { // Breakout
                         Show_Loading_Sequence("BREAKOUT");
                         Init_Breakout_Game();
                         ui_state = UI_BREAKOUT;
                         last_menu_index = -1;
                     } else if(menu_index == 3) { // Flappy
                         Show_Loading_Sequence("FLAPPY DROID");
                         Init_Flappy_Game();
                         ui_state = UI_FLAPPY;
                         last_menu_index = -1;
                     } else if(menu_index == 4) { // Racing
                         Show_Loading_Sequence("RACING");
                         Init_Racing_Game();
                         ui_state = UI_RACING;
                         last_menu_index = -1;
                     } else if(menu_index == 5) { // RunTiny
                         Show_Loading_Sequence("RUNTINY");
                         Init_RunTiny_Game();
                         ui_state = UI_RUNTINY;
                         last_menu_index = -1;
                     }
                 } else if(ui_state == UI_PLAYING) {
                     Stop_Music();
                     if(strncmp(current_playing_file, "welcome.wav", MAX_FILENAME_LEN) == 0) {
                         ui_state = UI_MENU_MAIN;
                         menu_index = 6; // Return to About
                         menu_mode = 0; // Reset mode
                     } else {
                         ui_state = UI_MENU_MUSIC_LIST;
                     }
                     last_menu_index = -1;
                 } else if(ui_state == UI_TEXT_VIEWER) {
                     ui_state = UI_MENU_TEXT_LIST;
                     last_menu_index = -1;
                 } else if(ui_state == UI_GAME) {
                     ui_state = UI_MENU_GAME_LIST;
                     menu_index = 0;
                     last_menu_index = -1;
                 } else if(ui_state == UI_BREAKOUT) {
                     ui_state = UI_MENU_GAME_LIST;
                     menu_index = 0;
                     last_menu_index = -1;
                 } else if(ui_state == UI_FLAPPY) {
                     if(flp_game_over) {
                        ui_state = UI_MENU_GAME_LIST;
                        menu_index = 0;
                        last_menu_index = -1;
                     }
                 } else if(ui_state == UI_RACING) {
                     if(race_game_over) {
                        ui_state = UI_MENU_GAME_LIST;
                        menu_index = 0;
                        last_menu_index = -1;
                     }
                 } else if(ui_state == UI_DEBUG_INPUT) {
                     ui_state = UI_MENU_MAIN;
                     last_menu_index = -1;
                 }
             }
        }
    }
    
    // Render UI
    // Check if redraw needed
    int redraw = 0;
    
    /* Blink Disabled
    if(HAL_GetTick() - last_blink_time > 500) {
        blink_state = !blink_state;
        last_blink_time = HAL_GetTick();
        redraw = 1;
    }
    */
    blink_state = 1;

    if(ui_state == UI_TEXT_VIEWER) {
        static int last_text_line = -1;
        if(current_text_line != last_text_line) {
            redraw = 1;
            last_text_line = current_text_line;
        }
    } else {
        if(menu_index != last_menu_index || menu_scroll != last_menu_scroll || ui_state == UI_PLAYING || ui_state == UI_GAME || ui_state == UI_BREAKOUT || ui_state == UI_FLAPPY || ui_state == UI_RACING || ui_state == UI_RUNTINY || ui_state == UI_DEBUG_INPUT || ui_state == UI_SETTINGS_CPU_SPEED || ui_state == UI_SETTINGS_CPU_JUMP || ui_state == UI_SETTINGS_DRAW_DENSITY || (current_draw_mode_idx == 1 && (ui_state == UI_MENU_GAME_LIST || ui_state == UI_SETTINGS || ui_state == UI_SETTINGS_DRAW_MODE))) {
            redraw = 1;
            blink_state = 1; // Reset blink on move
            // last_blink_time = HAL_GetTick();
        }
    }
    
    if(redraw && !(ui_state == UI_PLAYING && menu_mode == 1)) {
        // Save scroll offset before clearing
        saved_scroll_count = 0;
        
        if((ui_state == UI_GAME && game_over) || (ui_state == UI_BREAKOUT && brk_game_over) || (ui_state == UI_FLAPPY && flp_game_over) || (ui_state == UI_RACING && race_game_over) || (ui_state == UI_RUNTINY && run_game_over)) {
            if(ui_state == UI_BREAKOUT && brk_game_over == 2) SaveScroll("YOU WIN");
            else SaveScroll("GAME OVER");
            char s[32]; 
            if(ui_state == UI_GAME) sprintf(s, "SCORE: %d", game_score);
            else if(ui_state == UI_BREAKOUT) sprintf(s, "SCORE: %d", brk_score);
            else if(ui_state == UI_FLAPPY) sprintf(s, "SCORE: %d", flp_score);
            else if(ui_state == UI_RACING) sprintf(s, "SCORE: %d", race_score);
            else sprintf(s, "SCORE: %d", run_score);
            SaveScroll(s);
        } else if(ui_state == UI_MENU_MAIN || ui_state == UI_MENU_GAME_LIST || ui_state == UI_MENU_MUSIC_LIST || ui_state == UI_MENU_TEXT_LIST || ui_state == UI_SETTINGS || ui_state == UI_SETTINGS_DRAW_MODE) {
             // Determine items to save
             int count = 0;
             const char **items = NULL;
             
             if(ui_state == UI_MENU_MAIN) {
                 count = main_menu_count;
                 items = main_menu_items;
             } else if(ui_state == UI_MENU_GAME_LIST) {
                 count = game_menu_count;
                 items = game_menu_items;
             } else if(ui_state == UI_SETTINGS) {
                 count = settings_menu_count;
                 items = settings_menu_items;
             } else if(ui_state == UI_SETTINGS_DRAW_MODE) {
                 count = draw_mode_menu_count;
                 items = draw_mode_menu_items;
             } else {
                 count = music_file_count;
                 items = (const char**)music_files;
             }
             
             for(int i=0; i<visible_lines; i++) {
                 int item_idx = menu_scroll + i;
                 if(item_idx >= count) break;
                 SaveScroll(items[item_idx]);
             }
        }

        DRAW_Clear();
        
        if(ui_state == UI_DEBUG_INPUT) {
            DRAW_AddString("DEBUG INPUT", 100, 100, 3800, 15, 15);
            
            char buf[32];
            
            // Read Pins
            // Note: Assuming Pull-Up, so Pressed = 0 (RESET), Released = 1 (SET)
            // Or Pull-Down? Usually buttons are Pull-Up. Let's display the raw value.
            
            sprintf(buf, "UP(PE2): %d", HAL_GPIO_ReadPin(KEY_UP_GPIO_Port, KEY_UP_Pin));
            DRAW_AddString(buf, 100, 100, 3200, 15, 15);
            
            sprintf(buf, "DOWN(PE4): %d", HAL_GPIO_ReadPin(KEY_DOWN_GPIO_Port, KEY_DOWN_Pin));
            DRAW_AddString(buf, 100, 100, 2800, 15, 15);
            
            sprintf(buf, "LEFT(PE3): %d", HAL_GPIO_ReadPin(KEY_LEFT_GPIO_Port, KEY_LEFT_Pin));
            DRAW_AddString(buf, 100, 100, 2400, 15, 15);
            
            sprintf(buf, "RIGHT(PE5): %d", HAL_GPIO_ReadPin(KEY_RIGHT_GPIO_Port, KEY_RIGHT_Pin));
            DRAW_AddString(buf, 100, 100, 2000, 15, 15);
            
            sprintf(buf, "GAME DIR: %d", Game_Input_Dir);
            DRAW_AddString(buf, 100, 100, 1400, 15, 15);
            
            DRAW_AddString("[PRESS ENC TO EXIT]", 100, 100, 800, 10, 10);
            
        } else if(ui_state == UI_SETTINGS_CPU_SPEED) {
            DRAW_AddString("CPU DRAW SPEED", 100, 100, 3500, 15, 15);
            
            char buf[32];
            sprintf(buf, "DELAY: %d", DRAW_GetCPUDelay());
            DRAW_AddString(buf, 100, 100, 2500, 15, 15);
            
            DRAW_AddString("[TURN ENC TO ADJUST]", 100, 100, 1500, 10, 10);
            DRAW_AddString("[PRESS ENC TO EXIT]", 100, 100, 1000, 10, 10);
            
        } else if(ui_state == UI_SETTINGS_CPU_JUMP) {
            DRAW_AddString("CPU JUMP DWELL", 100, 100, 3500, 15, 15);
            
            char buf[32];
            sprintf(buf, "DWELL: %d", DRAW_GetCPUJumpDwell());
            DRAW_AddString(buf, 100, 100, 2500, 15, 15);
            
            DRAW_AddString("[TURN ENC TO ADJUST]", 100, 100, 1500, 10, 10);
            DRAW_AddString("[PRESS ENC TO EXIT]", 100, 100, 1000, 10, 10);
            
        } else if(ui_state == UI_SETTINGS_DRAW_DENSITY) {
            DRAW_AddString("DRAW DENSITY", 100, 100, 3500, 15, 15);
            
            char buf[32];
            sprintf(buf, "DENSITY: %d%%", DRAW_GetDrawDensity());
            DRAW_AddString(buf, 100, 100, 2500, 15, 15);
            DRAW_AddString("(100=NORM, >100=SLOW)", 100, 100, 2000, 10, 10);
            
            DRAW_AddString("[TURN ENC TO ADJUST]", 100, 100, 1500, 10, 10);
            DRAW_AddString("[PRESS ENC TO EXIT]", 100, 100, 1000, 10, 10);
            
        } else if(ui_state == UI_GAME) {
            // Draw Snake Game
            // Draw Border
            DRAW_AddRect(0, 0, 4095, 4095);
            
            // Draw Snake
            int cell_size = 4096 / SNAKE_GRID_SIZE;
            for(int i=0; i<snake_len; i++) {
                int x = snake_body[i].x * cell_size + (cell_size/2);
                int y = snake_body[i].y * cell_size + (cell_size/2);
                // Draw a small square or circle for each segment
                // Using AddRect for simplicity, centered
                int half_size = (cell_size / 2) - 10;
                DRAW_AddRect(x - half_size, y - half_size, 2*half_size, 2*half_size);
            }
            
            // Draw Food
            int fx = snake_food.x * cell_size + (cell_size/2);
            int fy = snake_food.y * cell_size + (cell_size/2);
            // Draw X for food
            int f_half = (cell_size / 2) - 20;
            DRAW_AddLine(fx - f_half, fy - f_half, fx + f_half, fy + f_half);
            DRAW_AddLine(fx - f_half, fy + f_half, fx + f_half, fy - f_half);
            
            if(game_over) {
                int16_t s1 = DRAW_AddString("GAME OVER", 100, 0, 2200, 15, 15);
                RestoreScroll(s1, "GAME OVER");
                
                char score_str[32];
                sprintf(score_str, "SCORE: %d   ", game_score); // Add spaces to ensure width > screen for scrolling
                int16_t s2 = DRAW_AddString(score_str, 100, 0, 1600, 15, 15);
                RestoreScroll(s2, score_str);
            }
        } else if(ui_state == UI_BREAKOUT) {
            // Draw Border
            DRAW_AddRect(0, 0, 4095, 4095);
            
            // Draw Paddle
            int py = 200;
            DRAW_AddRect(brk_paddle.x, py, BRK_PADDLE_W, BRK_PADDLE_H);
            
            // Draw Ball
            DRAW_AddCircle(brk_ball.x, brk_ball.y, BRK_BALL_R);
            
            // Draw Bricks
            int brick_start_y = 3000;
            for(int r=0; r<BRK_ROWS; r++) {
                for(int c=0; c<BRK_COLS; c++) {
                    if(brk_bricks[r][c]) {
                        int bx = c * BRK_BRICK_W;
                        int by = brick_start_y + (r * BRK_BRICK_H);
                        int bw = BRK_BRICK_W;
                        int bh = BRK_BRICK_H;
                        
                        // Bottom Edge
                        if(r == 0 || !brk_bricks[r-1][c]) {
                            DRAW_AddLine(bx, by, bx + bw, by);
                        }
                        
                        // Top Edge
                        if(r == BRK_ROWS-1 || !brk_bricks[r+1][c]) {
                            DRAW_AddLine(bx, by + bh, bx + bw, by + bh);
                        }
                        
                        // Left Edge
                        if(c == 0 || !brk_bricks[r][c-1]) {
                            DRAW_AddLine(bx, by, bx, by + bh);
                        }
                        
                        // Right Edge
                        if(c == BRK_COLS-1 || !brk_bricks[r][c+1]) {
                            DRAW_AddLine(bx + bw, by, bx + bw, by + bh);
                        }
                    }
                }
            }
            
            // Draw Score/Lives
            if(brk_game_over) {
                if(brk_game_over == 2) {
                    int16_t s1 = DRAW_AddString("YOU WIN", 100, 0, 2200, 15, 15);
                    RestoreScroll(s1, "YOU WIN");
                } else {
                    int16_t s1 = DRAW_AddString("GAME OVER", 100, 0, 2200, 15, 15);
                    RestoreScroll(s1, "GAME OVER");
                }
                
                char score_str[32];
                sprintf(score_str, "SCORE: %d   ", brk_score);
                int16_t s2 = DRAW_AddString(score_str, 100, 0, 1600, 15, 15);
                RestoreScroll(s2, score_str);
            } else {
                char lives_str[16];
                sprintf(lives_str, "L:%d", brk_lives);
                DRAW_AddString(lives_str, 100, 100, 100, 10, 10);
            }
        } else if(ui_state == UI_FLAPPY) {
            // Draw Border
            DRAW_AddRect(0, 0, 4095, 4095);
            
            // Draw Player (Droid Head)
            DRAW_AddCircle(FLP_PLAYER_X, flp_player.y, FLP_PLAYER_R);
            // Antennas
            DRAW_AddLine(FLP_PLAYER_X - 40, flp_player.y + 60, FLP_PLAYER_X - 80, flp_player.y + 150);
            DRAW_AddLine(FLP_PLAYER_X + 40, flp_player.y + 60, FLP_PLAYER_X + 80, flp_player.y + 150);
            
            // Draw Obstacles
            for(int i=0; i<FLP_MAX_OBSTACLES; i++) {
                if(flp_obstacles[i].active) {
                    int x = flp_obstacles[i].x;
                    int gap_y = flp_obstacles[i].gap_y;
                    int w = FLP_OBSTACLE_W;
                    int h_gap = FLP_GAP_H / 2;
                    
                    // Top Obstacle (from gap top to screen top)
                    int top_y = gap_y + h_gap;
                    if(top_y < 4096) {
                        DRAW_AddRect(x, top_y, w, 4096 - top_y);
                    }
                    
                    // Bottom Obstacle (from 0 to gap bottom)
                    int bot_y = gap_y - h_gap;
                    if(bot_y > 0) {
                        DRAW_AddRect(x, 0, w, bot_y);
                    }
                }
            }
            
            if(flp_game_over) {
                int16_t s1 = DRAW_AddString("GAME OVER", 100, 0, 2200, 15, 15);
                RestoreScroll(s1, "GAME OVER");
                
                char score_str[32];
                sprintf(score_str, "SCORE: %d   ", flp_score);
                int16_t s2 = DRAW_AddString(score_str, 100, 0, 1600, 15, 15);
                RestoreScroll(s2, score_str);
            } else {
                char score_str[16];
                sprintf(score_str, "%d", flp_score);
                DRAW_AddString(score_str, 100, 2000, 3800, 15, 15);
            }
        } else if(ui_state == UI_RACING) {
            // Draw Border
            DRAW_AddRect(0, 0, 4095, 4095);
            
            // Draw Car
            DRAW_AddRect(race_car.x, 200, RACE_CAR_W, RACE_CAR_H);
            // Add some detail to car (wheels)
            DRAW_AddRect(race_car.x - 50, 250, 50, 150);
            DRAW_AddRect(race_car.x + RACE_CAR_W, 250, 50, 150);
            DRAW_AddRect(race_car.x - 50, 500, 50, 150);
            DRAW_AddRect(race_car.x + RACE_CAR_W, 500, 50, 150);
            
            // Draw Obstacles
            for(int i=0; i<RACE_MAX_OBSTACLES; i++) {
                if(race_obstacles[i].active) {
                    DRAW_AddRect(race_obstacles[i].x, race_obstacles[i].y, RACE_OBSTACLE_W, RACE_OBSTACLE_H);
                    // Draw X inside obstacle
                    DRAW_AddLine(race_obstacles[i].x, race_obstacles[i].y, race_obstacles[i].x + RACE_OBSTACLE_W, race_obstacles[i].y + RACE_OBSTACLE_H);
                    DRAW_AddLine(race_obstacles[i].x, race_obstacles[i].y + RACE_OBSTACLE_H, race_obstacles[i].x + RACE_OBSTACLE_W, race_obstacles[i].y);
                }
            }
            
            if(race_game_over) {
                int16_t s1 = DRAW_AddString("GAME OVER", 100, 0, 2200, 15, 15);
                RestoreScroll(s1, "GAME OVER");
                
                char score_str[32];
                sprintf(score_str, "SCORE: %d   ", race_score);
                int16_t s2 = DRAW_AddString(score_str, 100, 0, 1600, 15, 15);
                RestoreScroll(s2, score_str);
            } else {
                char score_str[16];
                sprintf(score_str, "%d", race_score);
                DRAW_AddString(score_str, 100, 2000, 3500, 15, 15);
            }
        } else if(ui_state == UI_RUNTINY) {
            // Draw Border
            DRAW_AddRect(0, 0, 4095, 4095);
            
            // Draw Ground
            DRAW_AddLine(0, RUN_GROUND_Y, 4096, RUN_GROUND_Y);
            
            // Draw Player
            DRAW_AddRect(RUN_PLAYER_X, run_player.y, RUN_PLAYER_W, RUN_PLAYER_H);
            // Legs animation
            if(run_player.y == RUN_GROUND_Y) { // Only animate when running
                static int leg_state = 0;
                if(HAL_GetTick() % 200 < 100) leg_state = 0; else leg_state = 1;
                
                if(leg_state) {
                    DRAW_AddLine(RUN_PLAYER_X + 50, run_player.y, RUN_PLAYER_X + 50, run_player.y - 50);
                    DRAW_AddLine(RUN_PLAYER_X + 150, run_player.y, RUN_PLAYER_X + 150, run_player.y - 50);
                } else {
                    DRAW_AddLine(RUN_PLAYER_X + 20, run_player.y, RUN_PLAYER_X + 80, run_player.y - 50);
                    DRAW_AddLine(RUN_PLAYER_X + 180, run_player.y, RUN_PLAYER_X + 120, run_player.y - 50);
                }
            }
            
            // Draw Obstacles
            for(int i=0; i<RUN_MAX_OBSTACLES; i++) {
                if(run_obstacles[i].active) {
                    DRAW_AddRect(run_obstacles[i].x, RUN_GROUND_Y, RUN_OBSTACLE_W, RUN_OBSTACLE_H);
                }
            }
            
            if(run_game_over) {
                int16_t s1 = DRAW_AddString("GAME OVER", 100, 0, 2200, 15, 15);
                RestoreScroll(s1, "GAME OVER");
                
                char score_str[32];
                sprintf(score_str, "SCORE: %d   ", run_score);
                int16_t s2 = DRAW_AddString(score_str, 100, 0, 1600, 15, 15);
                RestoreScroll(s2, score_str);
            } else {
                char score_str[16];
                sprintf(score_str, "%d", run_score);
                DRAW_AddString(score_str, 100, 2000, 3500, 15, 15);
            }
        } else if(ui_state == UI_PLAYING) {
            DRAW_AddString("PLAYING:", 100, 100, 3500, 15, 15);
            DRAW_AddString(current_playing_file, 100, 100, 3000, 15, 15);
            
            char vol_str[16];
            sprintf(vol_str, "VOL: %d", volume);
            DRAW_AddString(vol_str, 100, 100, 2000, 15, 15);
            
            DRAW_AddString("[PRESS TO STOP]", 100, 100, 1000, 10, 10);
        } else if(ui_state == UI_TEXT_VIEWER) {
            // Render Text Lines
            for(int i=0; i<3; i++) {
                int line_idx = current_text_line + i;
                if(line_idx >= total_text_lines) break;
                
                int y_pos = start_y - (i * 1200);
                // Use larger font for text: 25% scale//调节字号
                DRAW_AddString(text_lines[line_idx], 50, 100, y_pos, 25, 25);
            }
        } else {
            // Menu Rendering
            int count = 0;
            const char **items = NULL;
            char (*file_items)[MAX_FILENAME_LEN] = NULL;
            
            if(ui_state == UI_MENU_MAIN) {
                count = main_menu_count;
                items = main_menu_items;
            } else if(ui_state == UI_MENU_GAME_LIST) {
                count = game_menu_count;
                items = game_menu_items;
            } else if(ui_state == UI_SETTINGS) {
                count = settings_menu_count;
                items = settings_menu_items;
            } else if(ui_state == UI_SETTINGS_DRAW_MODE) {
                count = draw_mode_menu_count;
                items = draw_mode_menu_items;
            } else {
                count = music_file_count;
                file_items = music_files;
            }
            
            // Calculate Scroll
            if(menu_index < menu_scroll) menu_scroll = menu_index;
            if(menu_index >= menu_scroll + visible_lines) menu_scroll = menu_index - visible_lines + 1;
            
            for(int i=0; i<visible_lines; i++) {
                int item_idx = menu_scroll + i;
                if(item_idx >= count) break;
                
                int y_pos = start_y - (i * line_height);
                
                const char *text;
                char display_text[64];
                if(items) {
                    text = items[item_idx];
                    if(ui_state == UI_SETTINGS && item_idx == 1) {
                        sprintf(display_text, "DRAW MODE: %s", current_draw_mode_idx ? "CPU" : "DMA");
                        text = display_text;
                    }
                    else if(ui_state == UI_SETTINGS_DRAW_MODE) {
                        if(item_idx == 1 && current_draw_mode_idx == 0) {
                            sprintf(display_text, "DMA [X]");
                            text = display_text;
                        } else if(item_idx == 2 && current_draw_mode_idx == 1) {
                            sprintf(display_text, "CPU [X]");
                            text = display_text;
                        }
                    }
                }
                else text = file_items[item_idx];
                
                // Draw Cursor
                if(item_idx == menu_index) {
                    if(blink_state) DRAW_AddString(">", 100, 100, y_pos, 15, 15);
                    
                    // Scrolling Text
                    int16_t slot = DRAW_AddString(text, 100, 400, y_pos, 15, 15);
                    RestoreScroll(slot, text);
                } else {
                    // Normal Text
                    int16_t slot = DRAW_AddString(text, 100, 400, y_pos, 15, 15);
                    RestoreScroll(slot, text);
                }
            }
        }
        
        if(ui_state != UI_TEXT_VIEWER) DRAW_AddRect(0, 0, 4095, 4095);
        
        last_menu_index = menu_index;
        last_menu_scroll = menu_scroll;
        
        if(redraw || current_draw_mode_idx == 1) {
            DRAW_Render();
        }
    }
    
    // Force continuous refresh for CPU mode to prevent static image fading
    if(current_draw_mode_idx == 1) {
        osDelay(1); // Minimal delay to allow other tasks
    } else {
        osDelay(10);
    }
  }
  /* USER CODE END GuiTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
void Scan_Music_Files(const char* path) {
    DIR dir;
    FILINFO fno;
    FRESULT res;
    
    // Clear file list
    memset(music_files, 0, sizeof(music_files));
    music_file_count = 0;
    
    // Add "Back" option
    strcpy(music_files[0], "Back");
    music_file_count++;
    
    res = f_opendir(&dir, path);
    if (res == FR_OK) {
        for (;;) {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == 0) break;
            if (fno.fattrib & AM_DIR) continue;
            
            // Check extension .wav
            if (strstr(fno.fname, ".wav") || strstr(fno.fname, ".WAV")) {
                if (music_file_count < MAX_MUSIC_FILES) {
                    strncpy(music_files[music_file_count], fno.fname, MAX_FILENAME_LEN - 1);
                    music_files[music_file_count][MAX_FILENAME_LEN - 1] = '\0';
                    music_file_count++;
                }
            }
        }
        f_closedir(&dir);
    }
}

void Play_Music(char* filename) {
    Stop_Music();
    Playback_Finished = 0;
    
    char path[64];
    if(filename[0] == '/') {
        snprintf(path, sizeof(path), "%s", filename);
    } else {
        snprintf(path, sizeof(path), "/music/%s", filename);
    }
    
    FRESULT res = f_open(&fil, path, FA_READ);
    if(res == FR_OK) {
        UINT br;
        uint8_t header[44];
        f_read(&fil, header, 44, &br);
        
        if(strncmp((char*)header, "RIFF", 4) == 0 && strncmp((char*)&header[8], "WAVE", 4) == 0) {
             uint32_t data_size = header[40] | (header[41] << 8) | (header[42] << 16) | (header[43] << 24);
             SD_Wave_Total_Data_Left = data_size;
             
             uint32_t to_read = (data_size > SD_WAVE_MAX_LEN) ? SD_WAVE_MAX_LEN : data_size;
             f_read(&fil, SD_Wave_Buffer, to_read, &br);
             
             SD_Wave_Idx = 0;
             SD_Wave_Write_Idx = (br < SD_WAVE_MAX_LEN) ? br : 0;
             SD_Wave_Total_Data_Left -= br;
             
             SD_Wave_Loaded = 1;
             
             // Set Frequency (Assuming 44.1kHz for now)
             __HAL_TIM_SET_PRESCALER(&htim3, 0);
             __HAL_TIM_SET_AUTORELOAD(&htim3, 1904);
             __HAL_TIM_SET_COUNTER(&htim3, 0);
        } else {
            f_close(&fil);
        }
    }
}

void Play_Video(char* filename) {
    Stop_Music();
    Playback_Finished = 0;
    
    char path[64];
    if(filename[0] == '/') {
        snprintf(path, sizeof(path), "%s", filename);
    } else {
        snprintf(path, sizeof(path), "/video/%s", filename);
    }
    
    FRESULT res = f_open(&fil, path, FA_READ);
    if(res == FR_OK) {
        UINT br;
        uint8_t header[44];
        f_read(&fil, header, 44, &br);
        
        if(strncmp((char*)header, "RIFF", 4) == 0 && strncmp((char*)&header[8], "WAVE", 4) == 0) {
             uint32_t data_size = header[40] | (header[41] << 8) | (header[42] << 16) | (header[43] << 24);
             SD_Wave_Total_Data_Left = data_size;
             
             uint32_t to_read = (data_size > SD_WAVE_MAX_LEN) ? SD_WAVE_MAX_LEN : data_size;
             f_read(&fil, SD_Wave_Buffer, to_read, &br);
             
             SD_Wave_Idx = 0;
             SD_Wave_Write_Idx = (br < SD_WAVE_MAX_LEN) ? br : 0;
             SD_Wave_Total_Data_Left -= br;
             
             SD_Wave_Loaded = 1;
             Video_Mode = 1;
             
             // --- Configure DAC for Immediate Output (No Trigger) ---
             extern DAC_HandleTypeDef hdac;
             HAL_DAC_Stop_DMA(&hdac, DAC_CHANNEL_1);
             HAL_DAC_Stop_DMA(&hdac, DAC_CHANNEL_2);
             HAL_DAC_Stop(&hdac, DAC_CHANNEL_1);
             HAL_DAC_Stop(&hdac, DAC_CHANNEL_2);
             
             DAC_ChannelConfTypeDef sConfig = {0};
             sConfig.DAC_Trigger = DAC_TRIGGER_NONE;
             sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_DISABLE;
             
             if (HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_1) != HAL_OK) {
                 DRAW_Terminal_Print("DAC1 CFG ERR\n");
             }
             if (HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_2) != HAL_OK) {
                 DRAW_Terminal_Print("DAC2 CFG ERR\n");
             }
             
             HAL_DAC_Start(&hdac, DAC_CHANNEL_1);
             HAL_DAC_Start(&hdac, DAC_CHANNEL_2);
             
             // Center Beam Initially
             HAL_DAC_SetValue(&hdac, DAC_CHANNEL_1, DAC_ALIGN_12B_R, 2048);
             HAL_DAC_SetValue(&hdac, DAC_CHANNEL_2, DAC_ALIGN_12B_R, 2048);
             // -----------------------------------------------------
             
             // Set Frequency (Assuming 44.1kHz for now)
             __HAL_TIM_SET_PRESCALER(&htim3, 0);
             __HAL_TIM_SET_AUTORELOAD(&htim3, 1904);
             __HAL_TIM_SET_COUNTER(&htim3, 0);
        } else {
            f_close(&fil);
        }
    }
}

void Stop_Music(void) {
    SD_Wave_Loaded = 0;
    
    if(Video_Mode) {
        // --- Restore DAC for Drawing (Timer 6 Trigger) ---
        extern DAC_HandleTypeDef hdac;
        HAL_DAC_Stop(&hdac, DAC_CHANNEL_1);
        HAL_DAC_Stop(&hdac, DAC_CHANNEL_2);
        
        DAC_ChannelConfTypeDef sConfig = {0};
        sConfig.DAC_Trigger = DAC_TRIGGER_T6_TRGO;
        sConfig.DAC_OutputBuffer = DAC_OUTPUTBUFFER_DISABLE;
        
        HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_1);
        HAL_DAC_ConfigChannel(&hdac, &sConfig, DAC_CHANNEL_2);
        // DMA will be restarted by GuiTask -> DRAW_Render
        // -------------------------------------------------
    }
    
    Video_Mode = 0;
    osDelay(10); // Wait for AudioTask to pause
    f_close(&fil);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_3, 0);
    __HAL_TIM_SET_COMPARE(&htim2, TIM_CHANNEL_1, 0);
}


void Scan_Text_Files(const char* path) {
    DIR dir;
    FILINFO fno;
    FRESULT res;
    
    // Reuse music_files array
    memset(music_files, 0, sizeof(music_files));
    music_file_count = 0;
    
    // Add "Back" option
    strcpy(music_files[0], "Back");
    music_file_count++;
    
    res = f_opendir(&dir, path);
    if (res == FR_OK) {
        for (;;) {
            res = f_readdir(&dir, &fno);
            if (res != FR_OK || fno.fname[0] == 0) break;
            if (fno.fattrib & AM_DIR) continue;
            
            // Check extension .txt
            if (strstr(fno.fname, ".txt") || strstr(fno.fname, ".TXT")) {
                if (music_file_count < MAX_MUSIC_FILES) {
                    strncpy(music_files[music_file_count], fno.fname, MAX_FILENAME_LEN - 1);
                    music_files[music_file_count][MAX_FILENAME_LEN - 1] = '\0';
                    music_file_count++;
                }
            }
        }
        f_closedir(&dir);
    }
}

void Open_Text_File(char* filename) {
    char path[64];
    sprintf(path, "/text/%s", filename);
    
    FRESULT res = f_open(&fil, path, FA_READ);
    if(res == FR_OK) {
        UINT br;
        // Read file with margin for expansion (Limit to 12KB)
        f_read(&fil, SD_Wave_Buffer, 12000, &br);
        SD_Wave_Buffer[br] = '\0'; // Null terminate
        f_close(&fil);
        
        // --- Auto Word Wrap Pass ---
        int col = 0;
        uint32_t len = br;
        for(int i=0; i<len; i++) {
            if(SD_Wave_Buffer[i] == '\n') {
                col = 0;
            } else if(SD_Wave_Buffer[i] == '\r') {
                // ignore
            } else {
                col++;
                if(col >= 7) { // Max chars per line (7 chars for scale 25)
                    // Insert newline
                    if(len < SD_WAVE_MAX_LEN - 2) {
                        // Shift data
                        memmove(&SD_Wave_Buffer[i+1], &SD_Wave_Buffer[i], len - i + 1); // +1 for null terminator
                        SD_Wave_Buffer[i] = '\n';
                        len++;
                        col = 0;
                    } else {
                        break; // Buffer full
                    }
                }
            }
        }
        // ---------------------------
        
        // Parse lines
        total_text_lines = 0;
        current_text_line = 0;
        
        char *p = (char*)SD_Wave_Buffer;
        text_lines[total_text_lines++] = p;
        
        while(*p && total_text_lines < MAX_TEXT_LINES) {
            if(*p == '\n') {
                *p = '\0'; // Replace newline with null
                // Handle Windows \r\n
                if(p > (char*)SD_Wave_Buffer && *(p-1) == '\r') *(p-1) = '\0';
                
                text_lines[total_text_lines++] = p + 1;
            }
            p++;
        }
    }
}

void Init_Snake_Game(void) {
    snake_len = 3;
    snake_body[0].x = 10; snake_body[0].y = 10;
    snake_body[1].x = 10; snake_body[1].y = 9;
    snake_body[2].x = 10; snake_body[2].y = 8;
    
    snake_dir = 0; // UP
    Game_Input_Dir = 0;
    
    // Random Food
    snake_food.x = rand() % SNAKE_GRID_SIZE;
    snake_food.y = rand() % SNAKE_GRID_SIZE;
    
    game_over = 0;
    game_score = 0;
    last_game_tick = HAL_GetTick();
}

void Update_Snake_Game(void) {
    if(game_over) return;
    
    // Update Direction from Input
    // Prevent 180 degree turns
    if(Game_Input_Dir == 0 && snake_dir != 1) snake_dir = 0;
    if(Game_Input_Dir == 1 && snake_dir != 0) snake_dir = 1;
    if(Game_Input_Dir == 2 && snake_dir != 3) snake_dir = 2;
    if(Game_Input_Dir == 3 && snake_dir != 2) snake_dir = 3;
    
    if(HAL_GetTick() - last_game_tick > 200) { // 200ms speed
        last_game_tick = HAL_GetTick();
        
        // Move Body
        for(int i=snake_len-1; i>0; i--) {
            snake_body[i] = snake_body[i-1];
        }
        
        // Move Head
        if(snake_dir == 0) snake_body[0].y++;
        if(snake_dir == 1) snake_body[0].y--;
        if(snake_dir == 2) snake_body[0].x--;
        if(snake_dir == 3) snake_body[0].x++;
        
        // Check Wall Collision
        if(snake_body[0].x < 0 || snake_body[0].x >= SNAKE_GRID_SIZE ||
           snake_body[0].y < 0 || snake_body[0].y >= SNAKE_GRID_SIZE) {
            game_over = 1;
        }
        
        // Check Self Collision
        for(int i=1; i<snake_len; i++) {
            if(snake_body[0].x == snake_body[i].x && snake_body[0].y == snake_body[i].y) {
                game_over = 1;
            }
        }
        
        // Check Food
        if(snake_body[0].x == snake_food.x && snake_body[0].y == snake_food.y) {
            if(snake_len < SNAKE_MAX_LEN) {
                snake_len++;
                game_score += 10;
            }
            // New Food
            snake_food.x = rand() % SNAKE_GRID_SIZE;
            snake_food.y = rand() % SNAKE_GRID_SIZE;
        }
    }
}

void Init_Breakout_Game(void) {
    // Reset Paddle
    brk_paddle.x = 2048 - (BRK_PADDLE_W / 2);

    // Reset Ball
    brk_ball.x = 2048;
    brk_ball.y = 1500;
    brk_ball.vx = 30; 
    brk_ball.vy = 30;

    // Reset Bricks
    brk_brick_count = 0;
    for(int r=0; r<BRK_ROWS; r++) {
        for(int c=0; c<BRK_COLS; c++) {
            brk_bricks[r][c] = 1;
            brk_brick_count++;
        }
    }

    brk_score = 0;
    brk_lives = 3;
    brk_game_over = 0;
}

void Update_Breakout_Game(int16_t encoder_delta) {
    if(brk_game_over) return;

    // Update Paddle
    brk_paddle.x += encoder_delta * 100; // Sensitivity
    if(brk_paddle.x < 0) brk_paddle.x = 0;
    if(brk_paddle.x > 4096 - BRK_PADDLE_W) brk_paddle.x = 4096 - BRK_PADDLE_W;

    // Update Ball
    brk_ball.x += brk_ball.vx;
    brk_ball.y += brk_ball.vy;

    // Wall Collisions (Left/Right)
    if(brk_ball.x <= 0) {
        brk_ball.x = 0;
        brk_ball.vx = -brk_ball.vx;
    }
    if(brk_ball.x >= 4096) {
        brk_ball.x = 4096;
        brk_ball.vx = -brk_ball.vx;
    }
    
    // Top Wall
    if(brk_ball.y >= 4096) {
        brk_ball.y = 4096;
        brk_ball.vy = -brk_ball.vy;
    }

    // Paddle Collision (Bottom)
    int paddle_y = 200;
    int paddle_top = paddle_y + BRK_PADDLE_H;
    
    // Only check collision if moving DOWN
    if(brk_ball.vy < 0) {
        // Expanded collision box for anti-tunneling
        // Check if ball is below the top of the paddle (plus radius)
        // And above a reasonable "too late" threshold (e.g. 50) to prevent catching balls that are already dead
        if(brk_ball.y <= paddle_top + BRK_BALL_R && brk_ball.y >= 50) {
            // Check X range
            if(brk_ball.x >= brk_paddle.x - BRK_BALL_R && brk_ball.x <= brk_paddle.x + BRK_PADDLE_W + BRK_BALL_R) {
                // Hit
                brk_ball.vy = abs(brk_ball.vy); // Force Up
                
                // Anti-Tunneling: Push ball to surface
                brk_ball.y = paddle_top + BRK_BALL_R + 1;
                
                // Add some english
                brk_ball.vx += (brk_ball.x - (brk_paddle.x + BRK_PADDLE_W/2)) / 10;
            }
        }
    }

    // Bottom Wall (Death)
    if(brk_ball.y <= 0) {
        brk_lives--;
        if(brk_lives <= 0) {
            brk_game_over = 1;
        } else {
            // Reset Ball
            brk_ball.x = brk_paddle.x + BRK_PADDLE_W/2;
            brk_ball.y = 1000;
            brk_ball.vx = 30;
            brk_ball.vy = 30;
        }
    }

    // Brick Collision
    int brick_start_y = 3000;
    
    // Check if ball is in brick area
    if(brk_ball.y >= brick_start_y && brk_ball.y < brick_start_y + (BRK_ROWS * BRK_BRICK_H)) {
        int row = (brk_ball.y - brick_start_y) / BRK_BRICK_H;
        int col = brk_ball.x / BRK_BRICK_W;
        
        if(row >= 0 && row < BRK_ROWS && col >= 0 && col < BRK_COLS) {
            if(brk_bricks[row][col]) {
                brk_bricks[row][col] = 0;
                brk_ball.vy = -brk_ball.vy;
                brk_score += 10;
                brk_brick_count--;
                if(brk_brick_count <= 0) brk_game_over = 2; // Win
            }
        }
    }
}

void Init_Flappy_Game(void) {
    flp_player.y = 2048;
    flp_player.vy = 0;
    
    flp_score = 0;
    flp_game_over = 0;
    
    // Init Obstacles
    for(int i=0; i<FLP_MAX_OBSTACLES; i++) {
        flp_obstacles[i].active = 1;
        flp_obstacles[i].x = 4096 + 500 + (i * FLP_OBSTACLE_SPACING);
        flp_obstacles[i].gap_y = 1000 + (rand() % 2096); // 1000 to 3096
        flp_obstacles[i].passed = 0;
    }
}

void Update_Flappy_Game(int jump_requested) {
    if(flp_game_over) return;
    
    // Physics
    if(jump_requested) {
        flp_player.vy = FLP_JUMP_FORCE;
    }
    
    flp_player.y += flp_player.vy;
    flp_player.vy -= FLP_GRAVITY;
    
    // Cap Velocity
    if(flp_player.vy < -50) flp_player.vy = -50;
    if(flp_player.vy > 50) flp_player.vy = 50;
    
    // Floor/Ceiling Collision
    if(flp_player.y < FLP_PLAYER_R) {
        flp_player.y = FLP_PLAYER_R;
        flp_game_over = 1;
    }
    if(flp_player.y > 4096 - FLP_PLAYER_R) {
        flp_player.y = 4096 - FLP_PLAYER_R;
        flp_player.vy = 0;
    }
    
    // Obstacles
    for(int i=0; i<FLP_MAX_OBSTACLES; i++) {
        flp_obstacles[i].x -= FLP_SPEED;
        
        // Recycle
        if(flp_obstacles[i].x < -FLP_OBSTACLE_W) {
            // Find max x to place after
            int max_x = 0;
            for(int j=0; j<FLP_MAX_OBSTACLES; j++) {
                if(flp_obstacles[j].x > max_x) max_x = flp_obstacles[j].x;
            }
            flp_obstacles[i].x = max_x + FLP_OBSTACLE_SPACING;
            flp_obstacles[i].gap_y = 1000 + (rand() % 2096);
            flp_obstacles[i].passed = 0;
        }
        
        // Collision
        int ox = flp_obstacles[i].x;
        int ow = FLP_OBSTACLE_W;
        int gap_top = flp_obstacles[i].gap_y + FLP_GAP_H/2;
        int gap_bot = flp_obstacles[i].gap_y - FLP_GAP_H/2;
        
        // Horizontal Check
        if(FLP_PLAYER_X + FLP_PLAYER_R > ox && FLP_PLAYER_X - FLP_PLAYER_R < ox + ow) {
            // Vertical Check (Hit Top or Hit Bottom)
            if(flp_player.y + FLP_PLAYER_R > gap_top || flp_player.y - FLP_PLAYER_R < gap_bot) {
                flp_game_over = 1;
            }
        }
        
        // Score
        if(!flp_obstacles[i].passed && flp_obstacles[i].x + ow < FLP_PLAYER_X - FLP_PLAYER_R) {
            flp_score++;
            flp_obstacles[i].passed = 1;
        }
    }
}

void Init_Racing_Game(void) {
    race_car.x = 2048 - (RACE_CAR_W / 2);
    race_score = 0;
    race_game_over = 0;
    
    // Init Obstacles
    for(int i=0; i<RACE_MAX_OBSTACLES; i++) {
        race_obstacles[i].active = 1;
        race_obstacles[i].x = rand() % (4096 - RACE_OBSTACLE_W);
        race_obstacles[i].y = 4096 + (i * 1500); // Spacing
        race_obstacles[i].passed = 0;
    }
}

void Update_Racing_Game(int16_t encoder_delta) {
    if(race_game_over) return;
    
    // Move Car
    race_car.x += encoder_delta * 150; // Sensitivity
    if(race_car.x < 0) race_car.x = 0;
    if(race_car.x > 4096 - RACE_CAR_W) race_car.x = 4096 - RACE_CAR_W;
    
    // Move Obstacles
    int current_speed = RACE_SPEED + (race_score * 0.5);
    if(current_speed > 150) current_speed = 150; // Cap speed

    for(int i=0; i<RACE_MAX_OBSTACLES; i++) {
        race_obstacles[i].y -= current_speed;
        
        // Recycle
        if(race_obstacles[i].y < -RACE_OBSTACLE_H) {
            // Find max y
            int max_y = 0;
            for(int j=0; j<RACE_MAX_OBSTACLES; j++) {
                if(race_obstacles[j].y > max_y) max_y = race_obstacles[j].y;
            }
            race_obstacles[i].y = max_y + 1500; // Spacing
            race_obstacles[i].x = rand() % (4096 - RACE_OBSTACLE_W);
            race_obstacles[i].passed = 0;
        }
        
        // Collision
        // Simple AABB
        if(race_car.x < race_obstacles[i].x + RACE_OBSTACLE_W &&
           race_car.x + RACE_CAR_W > race_obstacles[i].x &&
           200 < race_obstacles[i].y + RACE_OBSTACLE_H &&
           200 + RACE_CAR_H > race_obstacles[i].y) {
            race_game_over = 1;
        }
        
        // Score
        if(!race_obstacles[i].passed && race_obstacles[i].y + RACE_OBSTACLE_H < 200) {
            race_score++;
            race_obstacles[i].passed = 1;
        }
    }
    
    // Increase speed over time?
    // Maybe later
}

void Init_RunTiny_Game(void) {
    run_player.y = RUN_GROUND_Y;
    run_player.vy = 0;
    run_player.jumping = 0;
    
    run_score = 0;
    run_game_over = 0;
    
    // Init Obstacles
    for(int i=0; i<RUN_MAX_OBSTACLES; i++) {
        run_obstacles[i].active = 1;
        run_obstacles[i].x = 4096 + 1000 + (i * 2000) + (rand() % 800); // Spacing with jitter
        run_obstacles[i].passed = 0;
    }
}

void Update_RunTiny_Game(int jump_requested) {
    if(run_game_over) return;
    
    // Physics
    if(jump_requested && !run_player.jumping) {
        run_player.vy = RUN_JUMP_FORCE;
        run_player.jumping = 1;
    }
    
    run_player.y += run_player.vy;
    
    if(run_player.y > RUN_GROUND_Y) {
        run_player.vy -= RUN_GRAVITY;
    } else {
        run_player.y = RUN_GROUND_Y;
        run_player.vy = 0;
        run_player.jumping = 0;
    }
    
    // Move Obstacles
    int current_speed = RUN_SPEED + (run_score * 0.2);
    if(current_speed > 100) current_speed = 100;
    
    for(int i=0; i<RUN_MAX_OBSTACLES; i++) {
        run_obstacles[i].x -= current_speed;
        
        // Recycle
        if(run_obstacles[i].x < -RUN_OBSTACLE_W) {
            // Find max x
            int max_x = 0;
            for(int j=0; j<RUN_MAX_OBSTACLES; j++) {
                if(run_obstacles[j].x > max_x) max_x = run_obstacles[j].x;
            }
            // More random spacing: 1200 to 2700 (Base 1200 + Random 1500)
            run_obstacles[i].x = max_x + 1200 + (rand() % 1500);
            run_obstacles[i].passed = 0;
        }
        
        // Collision
        if(run_obstacles[i].x < RUN_PLAYER_X + RUN_PLAYER_W &&
           run_obstacles[i].x + RUN_OBSTACLE_W > RUN_PLAYER_X &&
           run_player.y < RUN_GROUND_Y + RUN_OBSTACLE_H) { // Simple height check
            run_game_over = 1;
        }
        
        // Score
        if(!run_obstacles[i].passed && run_obstacles[i].x + RUN_OBSTACLE_W < RUN_PLAYER_X) {
            run_score++;
            run_obstacles[i].passed = 1;
        }
    }
}

void Show_Boot_Sequence(void) {
    DRAW_Terminal_Init(12, 100); // Scale 12, Spacing 100
    
    DRAW_Terminal_Print("> SYSTEM BOOT...\n");
    osDelay(200);
    DRAW_Terminal_Print("> CPU: STM32F407 @ 168MHz\n");
    osDelay(100);
    DRAW_Terminal_Print("> RAM: 192KB OK\n");
    osDelay(100);
    DRAW_Terminal_Print("> DAC: INITIALIZED\n");
    osDelay(100);
    DRAW_Terminal_Print("> CHECKING SD CARD...\n");
    
    // Wait for SD (Timeout 3s)
    int timeout = 30;
    while(SD_Status == 0 && timeout > 0) {
        osDelay(100);
        timeout--;
    }
    
    if(SD_Status == 1) {
        DRAW_Terminal_Print("> SD CARD: MOUNTED [OK]\n");
    } else {
        char buf[32];
        sprintf(buf, "> SD CARD: FAILED [%d]\n", SD_Status);
        DRAW_Terminal_Print(buf);
    }
    osDelay(200);
    
    DRAW_Terminal_Print("> LOADING KERNEL...\n");
    osDelay(300);
    DRAW_Terminal_Print("> SYSTEM READY.\n");
    osDelay(500);
}

void Show_Loading_Sequence(const char* game_name) {
    DRAW_Terminal_Init(15, 100);
    char buf[64];
    
    sprintf(buf, "> LOADING %s...\n", game_name);
    DRAW_Terminal_Print(buf);
    osDelay(200);
    
    DRAW_Terminal_Print("> ALLOCATING MEMORY...\n");
    osDelay(100);
    
    DRAW_Terminal_Print("> INIT GRAPHICS...\n");
    osDelay(100);
    
    DRAW_Terminal_Print("> STARTING...\n");
    osDelay(300);
}
/* USER CODE END Application */
