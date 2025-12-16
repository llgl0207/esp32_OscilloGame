#include "draw.h"
#include "dac.h"
#include "tim.h"
#include <math.h>
#include <string.h>

// --- 硬件抽象实现 ---
#if DRAW_CFG_OUTPUT_TYPE == DRAW_OUTPUT_TYPE_DAC
  #define DRAW_OUT_INIT() do { \
      HAL_DAC_Start(&DRAW_DAC_HANDLE, DRAW_DAC_CH_X); \
      HAL_DAC_Start(&DRAW_DAC_HANDLE, DRAW_DAC_CH_Y); \
  } while(0)
  
  #define DRAW_OUT_STOP_DMA() do { \
      HAL_DAC_Stop_DMA(&DRAW_DAC_HANDLE, DRAW_DAC_CH_X); \
      HAL_DAC_Stop_DMA(&DRAW_DAC_HANDLE, DRAW_DAC_CH_Y); \
  } while(0)

  #define DRAW_OUT_START_DMA(buf_x, buf_y, count) do { \
      HAL_DAC_Start_DMA(&DRAW_DAC_HANDLE, DRAW_DAC_CH_X, (uint32_t*)(buf_x), (count), DAC_ALIGN_12B_R); \
      HAL_DAC_Start_DMA(&DRAW_DAC_HANDLE, DRAW_DAC_CH_Y, (uint32_t*)(buf_y), (count), DAC_ALIGN_12B_R); \
  } while(0)

  // 优化的 CPU 输出 (直接寄存器访问)
  #define DRAW_OUT_CPU(x, y) do { \
      DRAW_DAC_HANDLE.Instance->DHR12RD = ((uint32_t)(y) << 16) | (uint32_t)(x); \
  } while(0)

#else // PWM
  #define DRAW_OUT_INIT() do { \
      HAL_TIM_PWM_Start(&DRAW_PWM_TIM_X, DRAW_PWM_CH_X); \
      HAL_TIM_PWM_Start(&DRAW_PWM_TIM_Y, DRAW_PWM_CH_Y); \
  } while(0)

  #define DRAW_OUT_STOP_DMA() do { \
      HAL_TIM_PWM_Stop_DMA(&DRAW_PWM_TIM_X, DRAW_PWM_CH_X); \
      HAL_TIM_PWM_Stop_DMA(&DRAW_PWM_TIM_Y, DRAW_PWM_CH_Y); \
  } while(0)

  #define DRAW_OUT_START_DMA(buf_x, buf_y, count) do { \
      HAL_TIM_PWM_Start_DMA(&DRAW_PWM_TIM_X, DRAW_PWM_CH_X, (uint32_t*)(buf_x), (count)); \
      HAL_TIM_PWM_Start_DMA(&DRAW_PWM_TIM_Y, DRAW_PWM_CH_Y, (uint32_t*)(buf_y), (count)); \
  } while(0)

  #define DRAW_OUT_CPU(x, y) do { \
      DRAW_PWM_TIM_X.Instance->DRAW_PWM_CCR_X = (x); \
      DRAW_PWM_TIM_Y.Instance->DRAW_PWM_CCR_Y = (y); \
  } while(0)
#endif
// -------------------------------------------

// DMA 缓冲区
uint16_t DAC_Buff_X[DRAW_BUF_SIZE];
uint16_t DAC_Buff_Y[DRAW_BUF_SIZE];
uint32_t DAC_Buff_Count = 0;

typedef struct { int16_t x0,y0,x1,y1; } Line_t;

static uint8_t set_pattern_by_char(char c);
static void compute_pattern_minmax_x(const Line_t *p, uint8_t len, int32_t *minx, int32_t *maxx);

// 为了简洁，包含从 main.c 复制的一组紧凑模式 (A..Z)
// 在真正的库中，我们会更紧凑地存储这些模式或生成它们。
static const Line_t pattern_A[] = { {2, 1, 4 ,7},{4,7,6,1},{3,4,5,4}};
static const Line_t pattern_a[] = { {3, 5, 5, 5},{5, 5, 6 , 4 } ,{ 6 , 4,  6, 1},{ 6, 1, 3 ,1},{3,1,2,2},{2,2,3,3},{3,3,6,3}};
static const Line_t pattern_B[] = { {2,1,2,7},{2,7,4,7},{4,7,5,6},{5,6,5,5},{5,5,4,4},{4,4,2,4},{4,4,5,3},{5,3,5,2},{5,2,4,1},{4,1,2,1}};
static const Line_t pattern_b[] = { {2,7,2,1},{2,1,4,1},{4,1,5,2},{5,2,5,3},{5,3,4,4},{4,4,2,4}};
static const Line_t pattern_C[] = { {6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,2},{2,2,3,1},{3,1,5,1},{5,1,6,2}};
static const Line_t pattern_c[] = { {5,3,4,4},{4,4,3,4},{3,4,2,3},{2,3,2,2},{2,2,3,1},{3,1,4,1},{4,1,5,2}};
static const Line_t pattern_D[] = { {2,1,2,7},{2,7,4,7},{4,7,6,5},{6,5,6,3},{6,3,4,1},{4,1,2,1}};
static const Line_t pattern_d[] = { {5,7,5,1},{5,1,3,1},{3,1,2,2},{2,2,2,3},{2,3,3,4},{3,4,5,4}};
static const Line_t pattern_E[] = { {6,7,2,7},{2,7,2,1},{2,1,6,1},{2,4,5,4}};
static const Line_t pattern_e[] = { {6,1,3,1},{3,1,2,2},{2,2,2,4},{2,4,3,5},{3,5,4,5},{4,5,5,4},{5,4,4,3},{4,3,2,3}};
static const Line_t pattern_F[] = { {2,1,2,7},{2,7,6,7},{2,4,5,4}};
static const Line_t pattern_f[] = { {5,6,4.5,6},{4.5,6,4,5.5},{4,5.5,4,4},{3,4,5,4},{4,4,4,1}};
static const Line_t pattern_G[] = { {6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,2},{2,2,3,1},{3,1,5,1},{5,1,6,2},{6,2,6,4},{6,4,4,4}};
static const Line_t pattern_g[] = { {2,-1,4,-1},{4,-1,5,0},{5,0,5,3},{5,3,4,4},{4,4,3,4},{3,4,2,3},{2,3,2,2},{2,2,3,1},{3,1,4,1},{4,1,5,2}};
static const Line_t pattern_H[] = { {2,1,2,7},{6,1,6,7},{2,4,6,4}};
static const Line_t pattern_h[] = { {3,1,3,7},{3,1,3,3},{3,3,4,4},{4,4,5,4},{5,4,6,3},{6,3,6,1}};
static const Line_t pattern_I[] = { {3,1,5,1},{4,1,4,7},{3,7,5,7}};
static const Line_t pattern_i[] = { {4,6,4,5},{3,4,4,4},{4,4,4,1},{3.5,1,4.5,1}};
static const Line_t pattern_J[] = { {5.5,7,6.5,7},{6,6,6,2},{6,2,5,1},{5,1,4,1}};
static const Line_t pattern_j[] = { {5,7,5,6},{5,3,5,0},{5,0,4,-1}};
static const Line_t pattern_K[] = { {2,7,2,1},{2,4,5,1},{2,4,5,7}};
static const Line_t pattern_k[] = { {3,1,3,6},{3,3,5,4},{3,3,5,1}};
static const Line_t pattern_L[] = { {2,7,2,1},{2,1,6,1}};
static const Line_t pattern_l[] = { {3.5,6.5,4,7},{4,7,4,1},{4,1,4.5,1.5}};
static const Line_t pattern_M[] = { {2,1,2,7},{2,7,4,4},{4,4,6,7},{6,7,6,1}};
static const Line_t pattern_m[] = { {2,1,2,4},{2,4,3,5},{3,5,4,4},{4,4,4,1},{4,1,4,4},{4,4,5,5},{5,5,6,4},{6,4,6,1}};
static const Line_t pattern_N[] = { {2,1,2,7},{2,7,6,1},{6,1,6,7}};
static const Line_t pattern_n[] = { {2,1,2,5},{2,4,3,5},{3,5,4,5},{4,5,5,4},{5,4,5,1}};
static const Line_t pattern_O[] = { {3,1,5,1},{5,1,6,2},{6,2,6,6},{6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,2},{2,2,3,1}};
static const Line_t pattern_o[] = { {3,1,4,1},{4,1,5,2},{5,2,5,4},{5,4,4,5},{4,5,3,5},{3,5,2,4},{2,4,2,2},{2,2,3,1}};
static const Line_t pattern_P[] = { {2,1,2,7},{2,7,5,7},{5,7,6,6},{6,6,6,5},{6,5,5,4},{5,4,2,4}};
static const Line_t pattern_p[] = { {2,-1,2,4},{2,4,4,4},{4,4,5,3},{5,3,4,2},{4,2,2,1}};
static const Line_t pattern_Q[] = { {3,1,5,1},{5,1,6,2},{6,2,6,6},{6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,2},{2,2,3,1},{5,3,7,1}};
static const Line_t pattern_q[] = { {5,-1,5,4},{5,4,3,4},{3,4,2,3},{2,3,2,2},{2,2,3,1},{3,1,5,1}};
static const Line_t pattern_R[] = { {2,1,2,7},{2,7,5,7},{5,7,6,6},{6,6,6,5},{6,5,5,4},{5,4,2,4},{4,4,7,1}};
static const Line_t pattern_r[] = { {2.5,4,3,4},{3,4,3,1},{2.5,1,3.5,1},{3,3,4,4},{4,4,5,4},{5,4,6,3}};
static const Line_t pattern_S[] = { {6,6,5,7},{5,7,3,7},{3,7,2,6},{2,6,2,5},{2,5,3,4},{3,4,5,4},{5,4,6,3},{6,3,6,2},{6,2,5,1},{5,1,3,1},{3,1,2,2}};
static const Line_t pattern_s[] = { {3.75,3.00,3.00,3.75},{3.00,3.75,2.25,3.75},{2.25,3.75,1.50,3.00},{1.50,3.00,2.25,2.25},{2.25,2.25,3.00,2.25},{3.00,2.25,3.75,1.50},{3.75,1.50,3.00,0.75},{3.00,0.75,2.25,0.75},{2.25,0.75,1.50,1.50}};
static const Line_t pattern_T[] = { {2,7,6,7},{4,7,4,1}};
static const Line_t pattern_t[] = { {2,4,4,4},{3,5,3,1.5},{3,1.5,3.5,1},{3.5,1,4,1}};
static const Line_t pattern_U[] = { {2,7,2,2},{2,2,3,1},{3,1,5,1},{5,1,6,2},{6,2,6,7}};
static const Line_t pattern_u[] = { {2,4,2,2},{2,2,3,1},{3,1,4,1},{4,1,5,2},{5,4,5,1}};
static const Line_t pattern_V[] = { {2,7,4,1},{4,1,6,7}};
static const Line_t pattern_v[] = { {2,4,3.5,1},{3.5,1,5,4}};
static const Line_t pattern_W[] = { {2,7,2,1},{2,1,4,4},{4,4,6,1},{6,1,6,7}};
static const Line_t pattern_w[] = { {2,4,2,2},{2,2,3,1},{3,1,4,2},{4,2,4,4},{4,4,4,2},{4,2,5,1},{5,1,6,2},{6,2,6,4}};
static const Line_t pattern_X[] = { {2,7,6,1},{6,7,2,1}};
static const Line_t pattern_x[] = { {2,4,4,1},{4,4,2,1}};
static const Line_t pattern_Y[] = { {2,7,4,4},{6,7,4,4},{4,4,4,1}};
static const Line_t pattern_y[] = { {2,-1,5,4},{3.5,1.5,2,4}};
static const Line_t pattern_Z[] = { {2,7,6,7},{6,7,2,1},{2,1,6,1}};
static const Line_t pattern_z[] = { {2,4,4,4},{4,4,2,1},{2,1,4,1}};



// 数字 0-9
/*
static const Line_t pattern_0[] = { {1200,3295,3000,3295}, {3000,3295,3000,295}, {3000,295,1200,295}, {1200,295,1200,3295} };
static const Line_t pattern_1[] = { {2100,3295,2100,295} };
static const Line_t pattern_2[] = { {1200,3295,3000,3295}, {3000,3295,3000,1795}, {3000,1795,1200,1795}, {1200,1795,1200,295}, {1200,295,3000,295} };
static const Line_t pattern_3[] = { {1200,3295,3000,3295}, {3000,3295,3000,295}, {3000,295,1200,295}, {1200,1795,3000,1795} };
static const Line_t pattern_4[] = { {1200,3295,1200,1795}, {1200,1795,3000,1795}, {3000,3295,3000,295} };
static const Line_t pattern_5[] = { {3000,3295,1200,3295}, {1200,3295,1200,1795}, {1200,1795,3000,1795}, {3000,1795,3000,295}, {3000,295,1200,295} };
static const Line_t pattern_6[] = { {3000,3295,1200,3295}, {1200,3295,1200,295}, {1200,295,3000,295}, {3000,295,3000,1795}, {3000,1795,1200,1795} };
static const Line_t pattern_7[] = { {1200,3295,3000,3295}, {3000,3295,3000,295} };
static const Line_t pattern_8[] = { {1200,3295,3000,3295}, {3000,3295,3000,295}, {3000,295,1200,295}, {1200,295,1200,3295}, {1200,1795,3000,1795} };
static const Line_t pattern_9[] = { {3000,295,3000,3295}, {3000,3295,1200,3295}, {1200,3295,1200,1795}, {1200,1795,3000,1795} };
*/

static const Line_t pattern_0[] = { {3,6.5,3,1.5},{3,1.5,3.5,1},{3.5,1,5.5,1},{5.5,1,6,1.5},{6,1.5,6,6.5},{6,6.5,5.5,7},{5.5,7,3.5,7},{3.5,7,3,6.5} };
static const Line_t pattern_1[] = { {3,6,4,7},{4,7,4,1},{3,1,5,1} };
static const Line_t pattern_2[] = { {2,6,3,7},{3,7,5,7},{5,7,6,6},{6,6,2,1},{2,1,6,1} };
static const Line_t pattern_3[] = { {3,7,5,7},{5,7,6,5.5},{6,5.5,5,4},{5,4,3,4},{5,4,6,2.5},{6,2.5,5,1},{5,1,3,1} };
static const Line_t pattern_4[] = { {3,7,2,3},{2,3,6,3},{6,3,4,3},{4,1,4,7} };
static const Line_t pattern_5[] = { {6,7,3,7},{3,7,3,4.5},{3,4.5,3.5,4},{3.5,4,5.5,4},{5.5,4,6,3.5},{6,3.5,6,1.5},{6,1.5,5.5,1},{5.5,1,3,1} };
static const Line_t pattern_6[] = { {6,6.5,5.5,7},{5.5,7,3.5,7},{3.5,7,3,6.5},{3,6.5,3,1.5},{3,1.5,3.5,1},{3.5,1,5.5,1},{5.5,1,6,1.5},{6,1.5,6,3.5},{6,3.5,5.5,4},{5.5,4,3,4} };
static const Line_t pattern_7[] = { {3,7,6,7},{6,7,5,1} };
static const Line_t pattern_8[] = { {5.5,4,3.5,4},{3.5,4,3,3.5},{3,3.5,3.5,4},{3.5,4,3,4.5},{3,4.5,3,6.5},{3,6.5,3.5,7},{3.5,7,5.5,7},{5.5,7,6,6.5},{6,6.5,6,4.5},{6,4.5,5.5,4},{5.5,4,6,3.5},{6,3.5,6,1.5},{6,1.5,5.5,1},{5.5,1,3.5,1},{3.5,1,3,1.5},{3,1.5,3,3.5},{3,3.5,3.5,4} };
static const Line_t pattern_9[] = { {3.5,7,3,6.5},{3,6.5,3,4.5},{3,4.5,3.5,4},{3.5,4,5.5,4},{5.5,4,6,5.5},{6,1.5,6,5.5},{5.5,4,3.5,4},{3,4.5,3,6.5},{3,6.5,3.5,7},{3.5,7,5.5,7},{5.5,7,6,6.5},{6,6.5,6,4.5},{6,4.5,5.5,4},{5.5,4,3.5,4} };
// 符号
static const Line_t pattern_excl[] = { {2100,3295,2100,1295}, {2100,795,2100,295} }; // !
static const Line_t pattern_apos[] = { {2100,3295,2100,2295} }; // '
static const Line_t pattern_hash[] = { {1600,3295,1600,295}, {2600,3295,2600,295}, {1200,2295,3000,2295}, {1200,1295,3000,1295} }; // #
static const Line_t pattern_pct[] = { {1200,295,3000,3295}, {1400,3095,1600,3095}, {1600,3095,1600,2895}, {1600,2895,1400,2895}, {1400,2895,1400,3095}, {2600,695,2800,695}, {2800,695,2800,495}, {2800,495,2600,495}, {2600,495,2600,695} }; // % (简化圆)
static const Line_t pattern_caret[] = { {1200,1795,2100,3295}, {2100,3295,3000,1795} }; // ^
static const Line_t pattern_ast[] = { {1200,2795,3000,795}, {3000,2795,1200,795}, {2100,3295,2100,295}, {1200,1795,3000,1795} }; // *
static const Line_t pattern_under[] = { {1200,295,3000,295} }; // _
static const Line_t pattern_minus[] = { {1200,1795,3000,1795} }; // -
static const Line_t pattern_plus[] = { {2100,3295,2100,295}, {1200,1795,3000,1795} }; // +
static const Line_t pattern_eq[] = { {1200,2295,3000,2295}, {1200,1295,3000,1295} }; // =
static const Line_t pattern_bslash[] = { {1200,3295,3000,295} }; // \ (反斜杠)
static const Line_t pattern_fslash[] = { {1200,295,3000,3295} }; // /
static const Line_t pattern_lparen[] = { {2600,3295,1600,1795}, {1600,1795,2600,295} }; // (
static const Line_t pattern_rparen[] = { {1600,3295,2600,1795}, {2600,1795,1600,295} }; // )
static const Line_t pattern_lbrack[] = { {2600,3295,1600,3295}, {1600,3295,1600,295}, {1600,295,2600,295} }; // [
static const Line_t pattern_rbrack[] = { {1600,3295,2600,3295}, {2600,3295,2600,295}, {2600,295,1600,295} }; // ]
static const Line_t pattern_lbrace[] = { {2600,3295,2100,3295}, {2100,3295,2100,1795}, {2100,1795,1600,1795}, {2100,1795,2100,295}, {2100,295,2600,295} }; // {
static const Line_t pattern_rbrace[] = { {1600,3295,2100,3295}, {2100,3295,2100,1795}, {2100,1795,2600,1795}, {2100,1795,2100,295}, {2100,295,1600,295} }; // }
static const Line_t pattern_quote[] = { {1600,3295,1600,2295}, {2600,3295,2600,2295} }; // "
static const Line_t pattern_semi[] = { {2100,3295,2100,2795}, {2100,1295,1700,295} }; // ; (点 + 线)
static const Line_t pattern_colon[] = { {2100,3295,2100,2295}, {2100,1295,2100,295} }; // :
static const Line_t pattern_comma[] = { {2100,495,2100,295}, {2100,295,1700,95} }; // ,
static const Line_t pattern_period[] = { {1900,495,2300,495}, {2300,495,2300,295}, {2300,295,1900,295}, {1900,295,1900,495} }; // .
static const Line_t pattern_question[] = { {1200,2595,1200,2995}, {1200,2995,1800,3295}, {1800,3295,2400,3295}, {2400,3295,3000,2995}, {3000,2995,3000,2295}, {3000,2295,2100,1595}, {2100,1595,2100,1095}, {2100,595,2100,295} }; // ?
static const Line_t pattern_at[] = { {2600,1295,2200,1295}, {2200,1295,1800,1695}, {1800,1695,1800,2095}, {1800,2095,2200,2495}, {2200,2495,2600,2095}, {2600,2095,2600,1695}, {2600,1695,3000,1295}, {3000,1295,3000,2895}, {3000,2895,1400,2895}, {1400,2895,1400,895}, {1400,895,3000,895} }; // @
static const Line_t pattern_dollar[] = { {2600,3295,1600,3295}, {1600,3295,1600,2095}, {1600,2095,2600,2095}, {2600,2095,2600,895}, {2600,895,1600,895}, {2100,3695,2100,495} }; // $
static const Line_t pattern_lt[] = { {2600,3295,1200,1795}, {1200,1795,2600,295} }; // <
static const Line_t pattern_gt[] = { {1200,3295,2600,1795}, {2600,1795,1200,295} }; // >
static const Line_t pattern_pipe[] = { {2100,3295,2100,295} }; // |
static const Line_t pattern_tilde[] = { {1200,1295,1600,2295}, {1600,2295,2200,1295}, {2200,1295,2600,2295} }; // ~

static const Line_t * const patterns[] = {
  pattern_A, pattern_B, pattern_C, pattern_D, pattern_E, pattern_F, pattern_G,
  pattern_H, pattern_I, pattern_J, pattern_K, pattern_L, pattern_M, pattern_N,
  pattern_O, pattern_P, pattern_Q, pattern_R, pattern_S, pattern_T, pattern_U,
  pattern_V, pattern_W, pattern_X, pattern_Y, pattern_Z,
  pattern_0, pattern_1, pattern_2, pattern_3, pattern_4,
  pattern_5, pattern_6, pattern_7, pattern_8, pattern_9,
  pattern_excl, pattern_apos, pattern_hash, pattern_pct, pattern_caret,
  pattern_ast, pattern_under, pattern_minus, pattern_plus, pattern_eq,
  pattern_bslash, pattern_fslash, pattern_lparen, pattern_rparen,
  pattern_lbrack, pattern_rbrack, pattern_lbrace, pattern_rbrace,
  pattern_quote, pattern_semi, pattern_colon,
  pattern_comma, pattern_period, pattern_question,
  pattern_at, pattern_dollar, pattern_lt, pattern_gt, pattern_pipe, pattern_tilde,
  pattern_a, pattern_b, pattern_c, pattern_d, pattern_e, pattern_f, pattern_g,
  pattern_h, pattern_i, pattern_j, pattern_k, pattern_l, pattern_m, pattern_n,
  pattern_o, pattern_p, pattern_q, pattern_r, pattern_s, pattern_t, pattern_u,
  pattern_v, pattern_w, pattern_x, pattern_y, pattern_z
};
static const uint8_t pattern_lengths[] = {
  sizeof(pattern_A)/sizeof(pattern_A[0]), sizeof(pattern_B)/sizeof(pattern_B[0]), sizeof(pattern_C)/sizeof(pattern_C[0]),
  sizeof(pattern_D)/sizeof(pattern_D[0]), sizeof(pattern_E)/sizeof(pattern_E[0]), sizeof(pattern_F)/sizeof(pattern_F[0]),
  sizeof(pattern_G)/sizeof(pattern_G[0]), sizeof(pattern_H)/sizeof(pattern_H[0]), sizeof(pattern_I)/sizeof(pattern_I[0]),
  sizeof(pattern_J)/sizeof(pattern_J[0]), sizeof(pattern_K)/sizeof(pattern_K[0]), sizeof(pattern_L)/sizeof(pattern_L[0]),
  sizeof(pattern_M)/sizeof(pattern_M[0]), sizeof(pattern_N)/sizeof(pattern_N[0]), sizeof(pattern_O)/sizeof(pattern_O[0]),
  sizeof(pattern_P)/sizeof(pattern_P[0]), sizeof(pattern_Q)/sizeof(pattern_Q[0]), sizeof(pattern_R)/sizeof(pattern_R[0]),
  sizeof(pattern_S)/sizeof(pattern_S[0]), sizeof(pattern_T)/sizeof(pattern_T[0]), sizeof(pattern_U)/sizeof(pattern_U[0]),
  sizeof(pattern_V)/sizeof(pattern_V[0]), sizeof(pattern_W)/sizeof(pattern_W[0]), sizeof(pattern_X)/sizeof(pattern_X[0]),
  sizeof(pattern_Y)/sizeof(pattern_Y[0]), sizeof(pattern_Z)/sizeof(pattern_Z[0]),
  sizeof(pattern_0)/sizeof(pattern_0[0]), sizeof(pattern_1)/sizeof(pattern_1[0]), sizeof(pattern_2)/sizeof(pattern_2[0]),
  sizeof(pattern_3)/sizeof(pattern_3[0]), sizeof(pattern_4)/sizeof(pattern_4[0]), sizeof(pattern_5)/sizeof(pattern_5[0]),
  sizeof(pattern_6)/sizeof(pattern_6[0]), sizeof(pattern_7)/sizeof(pattern_7[0]), sizeof(pattern_8)/sizeof(pattern_8[0]),
  sizeof(pattern_9)/sizeof(pattern_9[0]),
  sizeof(pattern_excl)/sizeof(pattern_excl[0]), sizeof(pattern_apos)/sizeof(pattern_apos[0]), sizeof(pattern_hash)/sizeof(pattern_hash[0]),
  sizeof(pattern_pct)/sizeof(pattern_pct[0]), sizeof(pattern_caret)/sizeof(pattern_caret[0]), sizeof(pattern_ast)/sizeof(pattern_ast[0]),
  sizeof(pattern_under)/sizeof(pattern_under[0]), sizeof(pattern_minus)/sizeof(pattern_minus[0]), sizeof(pattern_plus)/sizeof(pattern_plus[0]),
  sizeof(pattern_eq)/sizeof(pattern_eq[0]), sizeof(pattern_bslash)/sizeof(pattern_bslash[0]), sizeof(pattern_fslash)/sizeof(pattern_fslash[0]),
  sizeof(pattern_lparen)/sizeof(pattern_lparen[0]), sizeof(pattern_rparen)/sizeof(pattern_rparen[0]), sizeof(pattern_lbrack)/sizeof(pattern_lbrack[0]),
  sizeof(pattern_rbrack)/sizeof(pattern_rbrack[0]), sizeof(pattern_lbrace)/sizeof(pattern_lbrace[0]), sizeof(pattern_rbrace)/sizeof(pattern_rbrace[0]),
  sizeof(pattern_quote)/sizeof(pattern_quote[0]), sizeof(pattern_semi)/sizeof(pattern_semi[0]), sizeof(pattern_colon)/sizeof(pattern_colon[0]),
  sizeof(pattern_comma)/sizeof(pattern_comma[0]), sizeof(pattern_period)/sizeof(pattern_period[0]), sizeof(pattern_question)/sizeof(pattern_question[0]),
  sizeof(pattern_at)/sizeof(pattern_at[0]), sizeof(pattern_dollar)/sizeof(pattern_dollar[0]), sizeof(pattern_lt)/sizeof(pattern_lt[0]),
  sizeof(pattern_gt)/sizeof(pattern_gt[0]), sizeof(pattern_pipe)/sizeof(pattern_pipe[0]), sizeof(pattern_tilde)/sizeof(pattern_tilde[0]),
  sizeof(pattern_a)/sizeof(pattern_a[0]), sizeof(pattern_b)/sizeof(pattern_b[0]), sizeof(pattern_c)/sizeof(pattern_c[0]),
  sizeof(pattern_d)/sizeof(pattern_d[0]), sizeof(pattern_e)/sizeof(pattern_e[0]), sizeof(pattern_f)/sizeof(pattern_f[0]),
  sizeof(pattern_g)/sizeof(pattern_g[0]), sizeof(pattern_h)/sizeof(pattern_h[0]), sizeof(pattern_i)/sizeof(pattern_i[0]),
  sizeof(pattern_j)/sizeof(pattern_j[0]), sizeof(pattern_k)/sizeof(pattern_k[0]), sizeof(pattern_l)/sizeof(pattern_l[0]),
  sizeof(pattern_m)/sizeof(pattern_m[0]), sizeof(pattern_n)/sizeof(pattern_n[0]), sizeof(pattern_o)/sizeof(pattern_o[0]),
  sizeof(pattern_p)/sizeof(pattern_p[0]), sizeof(pattern_q)/sizeof(pattern_q[0]), sizeof(pattern_r)/sizeof(pattern_r[0]),
  sizeof(pattern_s)/sizeof(pattern_s[0]), sizeof(pattern_t)/sizeof(pattern_t[0]), sizeof(pattern_u)/sizeof(pattern_u[0]),
  sizeof(pattern_v)/sizeof(pattern_v[0]), sizeof(pattern_w)/sizeof(pattern_w[0]), sizeof(pattern_x)/sizeof(pattern_x[0]),
  sizeof(pattern_y)/sizeof(pattern_y[0]), sizeof(pattern_z)/sizeof(pattern_z[0])
};
static const uint8_t patterns_count = sizeof(patterns)/sizeof(patterns[0]);

static uint8_t pattern_index = 0;
static const Line_t *current_pattern = NULL;
static uint8_t current_pattern_length = 0;

// 变换：缩放 (百分比) 和偏移 (以 DAC 单位)
static uint16_t scale_x_pct = 100;
static uint16_t scale_y_pct = 100;
static int32_t offset_x = 0;
static int32_t offset_y = 0;

// 内存池设置
#define MAX_DRAW_OBJS 40
#define MAX_STR_LEN 64

typedef struct {
  uint8_t active;
  DrawType type;
  union {
      struct {
          char text[MAX_STR_LEN];
          int32_t x, y;
          uint16_t sx, sy;
          uint16_t spacing;
          int32_t scroll_offset;
          uint32_t last_scroll_time;
          int32_t total_width;
          int32_t view_width;
      } text_data;
      struct {
          int32_t x0, y0, x1, y1;
      } line_data;
      struct {
          int32_t x, y, w, h;
      } rect_data;
      struct {
          int32_t x, y, r;
      } circle_data;
  } data;
} DrawObj;

static DrawObj draw_pool[MAX_DRAW_OBJS];

// 终端状态
static uint16_t term_scale = 10;
static int32_t term_line_height = 400;
static int32_t term_char_spacing = 100;
static int8_t term_current_line = 0;
static int32_t term_cursor_x = 0;
static int32_t term_cursor_y = 4096; // 从顶部开始 (Y 是反转的吗？不，通常 0 是底部或顶部，取决于 DAC。暂时假设 4096 是顶部，稍后调整)
// 实际上，在之前的代码中：ty0 = (oy0 * scale) + offset_y。
// 如果 offset_y 为 0，它会在底部绘制吗？
// 让我们检查模式坐标。pattern_A: {0,0,2048,4096}。Y 从 0 到 4096。
// 所以 0 是底部，4096 是顶部。
// 终端应该从顶部 (4096) 开始并向下移动。

static uint8_t term_max_lines = MAX_DRAW_OBJS;

// 助手：计算模式最小/最大 X
static void compute_pattern_minmax_x(const Line_t *p, uint8_t len, int32_t *minx, int32_t *maxx){
  int32_t mn = 4096, mx = 0;
  uint8_t i;
  for(i=0;i<len;i++){
    if((int32_t)p[i].x0 < mn) mn = p[i].x0;
    if((int32_t)p[i].x1 < mn) mn = p[i].x1;
    if((int32_t)p[i].x0 > mx) mx = p[i].x0;
    if((int32_t)p[i].x1 > mx) mx = p[i].x1;
  }
  *minx = mn; *maxx = mx;
}

// 根据字符设置模式 (A-Z, 0-9, 符号)。如果设置成功返回 1，否则返回 0
static uint8_t set_pattern_by_char(char c){
  uint8_t idx = 255;
  if(c>='A' && c<='Z'){
    idx = (uint8_t)(c - 'A');
  } else if(c>='a' && c<='z'){
    idx = (uint8_t)(c - 'a') + 66;
  } else if(c>='0' && c<='9'){
    idx = (uint8_t)(c - '0') + 26;
  } else {
    switch(c){
      case '!': idx = 36; break;
      case '\'': idx = 37; break;
      case '#': idx = 38; break;
      case '%': idx = 39; break;
      case '^': idx = 40; break;
      case '*': idx = 41; break;
      case '_': idx = 42; break;
      case '-': idx = 43; break;
      case '+': idx = 44; break;
      case '=': idx = 45; break;
      case '\\': idx = 46; break;
      case '/': idx = 47; break;
      case '(': idx = 48; break;
      case ')': idx = 49; break;
      case '[': idx = 50; break;
      case ']': idx = 51; break;
      case '{': idx = 52; break;
      case '}': idx = 53; break;
      case '"': idx = 54; break;
      case ';': idx = 55; break;
      case ':': idx = 56; break;
      case ',': idx = 57; break;
      case '.': idx = 58; break;
      case '?': idx = 59; break;
      case '@': idx = 60; break;
      case '$': idx = 61; break;
      case '<': idx = 62; break;
      case '>': idx = 63; break;
      case '|': idx = 64; break;
      case '~': idx = 65; break;
    }
  }
  
  if(idx < patterns_count){
    pattern_index = idx;
    current_pattern = patterns[pattern_index];
    current_pattern_length = pattern_lengths[pattern_index];
    if(current_pattern_length == 0) current_pattern_length = 1;
    return 1;
  }
  return 0;
}

void DRAW_Update(void){
    uint32_t now = HAL_GetTick();
    uint8_t need_render = 0;
    
    for(int i=0; i<MAX_DRAW_OBJS; i++){
        if(draw_pool[i].active && draw_pool[i].type == DRAW_TYPE_TEXT){
            // 检查是否需要滚动
            if(draw_pool[i].data.text_data.total_width > draw_pool[i].data.text_data.view_width){
                if(now - draw_pool[i].data.text_data.last_scroll_time > 50){ // 50ms 更新率
                    draw_pool[i].data.text_data.last_scroll_time = now;
                    draw_pool[i].data.text_data.scroll_offset += 30; // 滚动速度
                    
                    // 循环
                    if(draw_pool[i].data.text_data.scroll_offset > draw_pool[i].data.text_data.total_width + 500){
                        draw_pool[i].data.text_data.scroll_offset = -draw_pool[i].data.text_data.view_width;
                    }
                    need_render = 1;
                }
            }
        }
    }
    
    if(need_render){
        DRAW_Render();
    }
}

static DrawMode current_draw_mode = DRAW_MODE_DMA;
static uint32_t cpu_draw_delay = 10; // CPU 绘图速度的调整参数
static uint32_t cpu_jump_dwell = 0; // CPU 跳转等待时间的调整参数
static uint32_t draw_density = 100; // 100 = 1.0x (正常), 200 = 2.0x (更慢/更亮), 50 = 0.5x (更快)

void DRAW_SetCPUDelay(uint32_t delay){
    cpu_draw_delay = delay;
    
    // 将预分频器设置为 0 以获得最大分辨率
    __HAL_TIM_SET_PRESCALER(&DRAW_CFG_DELAY_TIM, 0);
    
    // 将延迟映射到 ARR。
    // 之前：PSC=40, ARR=4 => ~2.5us
    // 现在：PSC=0。要获得 2.5us，ARR ~= 210。
    // 为了在高密度下允许更快的速度，我们从较低的值开始。
    // delay=0   -> ARR=20  (~0.24us) -> 比以前快 10 倍
    // delay=10  -> ARR=220 (~2.6us)  -> 类似于之前的最大速度
    // delay=100 -> ARR=2020 (~24us)
    
    uint32_t arr = 20 + (delay * 20);
    if(arr > 65535) arr = 65535;
    
    __HAL_TIM_SET_AUTORELOAD(&DRAW_CFG_DELAY_TIM, arr);
}

uint32_t DRAW_GetCPUDelay(void){
    return cpu_draw_delay;
}

void DRAW_SetCPUJumpDwell(uint32_t dwell){
    cpu_jump_dwell = dwell;
}

uint32_t DRAW_GetCPUJumpDwell(void){
    return cpu_jump_dwell;
}

void DRAW_SetDrawDensity(uint32_t density){
    if(density < 1) density = 1;
    draw_density = density;
}

uint32_t DRAW_GetDrawDensity(void){
    return draw_density;
}

void DRAW_SetMode(DrawMode mode){
    current_draw_mode = mode;
    if(mode == DRAW_MODE_CPU){
        // 如果切换到 CPU，停止 DMA
        DRAW_OUT_STOP_DMA();
        DRAW_OUT_INIT();
    }
}

static void DRAW_Render_CPU(void){
    // 启动定时器进行时序控制
    HAL_TIM_Base_Start(&DRAW_CFG_DELAY_TIM);

    // 遍历对象并直接绘制到 DAC
    for(int i=0; i<MAX_DRAW_OBJS; i++){
        if(!draw_pool[i].active) continue;
        
        if(draw_pool[i].type == DRAW_TYPE_TEXT)
        {
            int32_t cursor_x = draw_pool[i].data.text_data.x;
            int32_t cursor_y = draw_pool[i].data.text_data.y;
            uint16_t sx = draw_pool[i].data.text_data.sx;
            uint16_t sy = draw_pool[i].data.text_data.sy;
            int32_t scroll = draw_pool[i].data.text_data.scroll_offset;
            
            for(int c=0; c<MAX_STR_LEN; c++){
                char ch = draw_pool[i].data.text_data.text[c];
                if(ch == 0) break;
                if(ch == ' '){
                    cursor_x += (2000 * (int32_t)sx) / 100 + draw_pool[i].data.text_data.spacing;
                    continue;
                }
                
                if(set_pattern_by_char(ch)){
                    int32_t minx, maxx;
                    compute_pattern_minmax_x(current_pattern, current_pattern_length, &minx, &maxx);
                    
                    int32_t pre_scale = 1;
                    if((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')){
                        pre_scale = 512;
                    }
                    
                    minx *= pre_scale;
                    maxx *= pre_scale;
                    
                    int32_t char_w = ((maxx - minx) * (int32_t)sx) / 100;
                    int32_t draw_x = cursor_x - scroll;
                    
                    if(draw_x + char_w < 0) {
                            cursor_x += char_w + draw_pool[i].data.text_data.spacing;
                            continue; 
                    }
                    if(draw_x > 4096) {
                            break; 
                    }

                    for(int l=0; l<current_pattern_length; l++){
                        int32_t x0 = current_pattern[l].x0 * pre_scale;
                        int32_t y0 = current_pattern[l].y0 * pre_scale;
                        int32_t x1 = current_pattern[l].x1 * pre_scale;
                        int32_t y1 = current_pattern[l].y1 * pre_scale;
                        
                        int32_t tx0 = (x0 * (int32_t)sx) / 100 + cursor_x - (minx * (int32_t)sx) / 100 - scroll;
                        int32_t ty0 = (y0 * (int32_t)sy) / 100 + cursor_y;
                        int32_t tx1 = (x1 * (int32_t)sx) / 100 + cursor_x - (minx * (int32_t)sx) / 100 - scroll;
                        int32_t ty1 = (y1 * (int32_t)sy) / 100 + cursor_y;
                        
                        // 移动到起点并等待（跳转）
                        if(tx0 < 0) tx0 = 0; if(tx0 > 4095) tx0 = 4095;
                        if(ty0 < 0) ty0 = 0; if(ty0 > 4095) ty0 = 4095;
                        
                        int32_t dx = tx1 - tx0;
                        int32_t dy = ty1 - ty0;
                        
                        // 使用更高的分辨率进行 CPU 绘图以使其平滑
                        // 密度控制：steps = distance * (density / 100)
                        int steps = (int)(sqrtf((float)dx*dx + (float)dy*dy) * (float)draw_density / 100.0f); 
                        if(steps < 2) steps = 2;

                        // 直接寄存器访问以提高速度
                        DRAW_OUT_CPU(tx0, ty0);
                        
                        // 在起点停留以允许光束稳定（隐藏跳转线）
                        if(cpu_jump_dwell > 0) {
                            for(volatile int w=0; w<cpu_jump_dwell; w++);
                        }
                        
                        for(int s=1; s<=steps; s++){
                            int32_t px = tx0 + (dx * s) / steps;
                            int32_t py = ty0 + (dy * s) / steps;
                            
                            if(px < 0) px = 0; if(px > 4095) px = 4095;
                            if(py < 0) py = 0; if(py > 4095) py = 4095;
                            
                            DRAW_OUT_CPU(px, py);
                            
                            // 等待定时器更新标志
                            __HAL_TIM_CLEAR_FLAG(&DRAW_CFG_DELAY_TIM, TIM_FLAG_UPDATE);
                            while(__HAL_TIM_GET_FLAG(&DRAW_CFG_DELAY_TIM, TIM_FLAG_UPDATE) == RESET);
                        }
                    }
                    cursor_x += char_w + draw_pool[i].data.text_data.spacing;
                }
            }
        }
        else if(draw_pool[i].type == DRAW_TYPE_LINE)
        {
            int32_t x0 = draw_pool[i].data.line_data.x0;
            int32_t y0 = draw_pool[i].data.line_data.y0;
            int32_t x1 = draw_pool[i].data.line_data.x1;
            int32_t y1 = draw_pool[i].data.line_data.y1;
            
            int32_t dx = x1 - x0;
            int32_t dy = y1 - y0;
            int steps = (int)(sqrtf((float)dx*dx + (float)dy*dy) * (float)draw_density / 100.0f);
            if(steps < 2) steps = 2;

            DRAW_OUT_CPU(x0, y0);
            if(cpu_jump_dwell > 0) {
                for(volatile int w=0; w<cpu_jump_dwell; w++);
            }
            
            for(int s=1; s<=steps; s++){
                int32_t px = x0 + (dx * s) / steps;
                int32_t py = y0 + (dy * s) / steps;
                if(px > 4095) px = 4095; if(py > 4095) py = 4095;
                DRAW_OUT_CPU(px, py);
                
                // 等待定时器更新标志
                __HAL_TIM_CLEAR_FLAG(&DRAW_CFG_DELAY_TIM, TIM_FLAG_UPDATE);
                while(__HAL_TIM_GET_FLAG(&DRAW_CFG_DELAY_TIM, TIM_FLAG_UPDATE) == RESET);
            }
        }
        else if(draw_pool[i].type == DRAW_TYPE_RECT)
        {
            int32_t x = draw_pool[i].data.rect_data.x;
            int32_t y = draw_pool[i].data.rect_data.y;
            int32_t w = draw_pool[i].data.rect_data.w;
            int32_t h = draw_pool[i].data.rect_data.h;
            
            int32_t pts[5][2] = { {x,y}, {x+w,y}, {x+w,y+h}, {x,y+h}, {x,y} };
            
            // 移动到起点
            int32_t dx_init = pts[1][0] - pts[0][0];
            int32_t dy_init = pts[1][1] - pts[0][1];
            // 预计算第一步以避免启动延迟
            int steps_init = (int)(sqrtf((float)dx_init*dx_init + (float)dy_init*dy_init) * (float)draw_density / 100.0f);
            if(steps_init < 2) steps_init = 2;
            
            DRAW_OUT_CPU(x, y);
            
            // 在起点停留以允许光束稳定（隐藏跳转线）
            // 如果需要，使用 Timer 14 进行精确停留，或者只是忙等待
            if(cpu_jump_dwell > 0) {
                // 使用 TIM14 停留？不，cpu_jump_dwell 是当前 UI 中的原始循环计数。
                // 暂时保持简单的循环，但也许用户需要增加它。
                for(volatile int w=0; w<cpu_jump_dwell; w++);
            } else {
                // 即使为 0，如果跳转线可见，我们也可能需要微小的等待以让 DAC 稳定
                // 但用户要求“无延迟”。
                // 为了隐藏跳转线，我们实际上需要在目的地等待，以便光束
                // 在起点花费的时间比移动的时间多。
                // 如果我们立即开始绘制，移动时间就是占空比的一部分。
            }

            for(int l=0; l<4; l++){
                int32_t x0 = pts[l][0];
                int32_t y0 = pts[l][1];
                int32_t x1 = pts[l+1][0];
                int32_t y1 = pts[l+1][1];
                
                int32_t dx = x1 - x0;
                int32_t dy = y1 - y0;
                
                int steps;
                if(l==0) steps = steps_init;
                else {
                    steps = (int)(sqrtf((float)dx*dx + (float)dy*dy) * (float)draw_density / 100.0f);
                    if(steps < 2) steps = 2;
                }
                
                // 如果不是第一行，我们已经在 x0,y0。
                // 但为了一致性和处理角落停留（如果需要，未实现），我们只是绘制。
                // 实际上，对于矩形，我们已经在上一次迭代结束时的 x0,y0。
                // 所以我们不需要跳转。
                
                for(int s=1; s<=steps; s++){
                    int32_t px = x0 + (dx * s) / steps;
                    int32_t py = y0 + (dy * s) / steps;
                    if(px > 4095) px = 4095; if(py > 4095) py = 4095;
                    DRAW_OUT_CPU(px, py);
                    
                    // 等待定时器更新标志
                    __HAL_TIM_CLEAR_FLAG(&DRAW_CFG_DELAY_TIM, TIM_FLAG_UPDATE);
                    while(__HAL_TIM_GET_FLAG(&DRAW_CFG_DELAY_TIM, TIM_FLAG_UPDATE) == RESET);
                }
            }
        }
        else if(draw_pool[i].type == DRAW_TYPE_CIRCLE)
        {
            int32_t cx = draw_pool[i].data.circle_data.x;
            int32_t cy = draw_pool[i].data.circle_data.y;
            int32_t r = draw_pool[i].data.circle_data.r;
            
            int steps = (int)(6.28f * r * (float)draw_density / 100.0f); 
            if(steps < 10) steps = 10;
            
            // 移动到起点
            int32_t sx = cx + r;
            int32_t sy = cy;
            if(sx > 4095) sx = 4095;
            
            DRAW_OUT_CPU(sx, sy);
            if(cpu_jump_dwell > 0) {
                for(volatile int w=0; w<cpu_jump_dwell; w++);
            }
            
            for(int s=1; s<=steps; s++){
                float angle = (float)s / steps * 6.283185307f;
                int32_t px = cx + (int32_t)(cosf(angle) * r);
                int32_t py = cy + (int32_t)(sinf(angle) * r);
                if(px > 4095) px = 4095; if(py > 4095) py = 4095;
                DRAW_OUT_CPU(px, py);
                
                // 等待定时器更新标志
                __HAL_TIM_CLEAR_FLAG(&DRAW_CFG_DELAY_TIM, TIM_FLAG_UPDATE);
                while(__HAL_TIM_GET_FLAG(&DRAW_CFG_DELAY_TIM, TIM_FLAG_UPDATE) == RESET);
            }
        }
    }
    HAL_TIM_Base_Stop(&DRAW_CFG_DELAY_TIM);
}

void DRAW_Render(void){
    if(current_draw_mode == DRAW_MODE_CPU){
        DRAW_Render_CPU();
        return;
    }

    DAC_Buff_Count = 0;
    
    // 如果没有对象，输出中心点
    int active_found = 0;
    for(int i=0; i<MAX_DRAW_OBJS; i++){
        if(draw_pool[i].active) { active_found = 1; break; }
    }
    
    if(!active_found){
        // 用中心点填充
        for(int i=0; i<100; i++){ // 最小缓冲区
             DAC_Buff_X[i] = 2048;
             DAC_Buff_Y[i] = 2048;
        }
        DAC_Buff_Count = 100;
    } else {
        // 渲染对象
        for(int i=0; i<MAX_DRAW_OBJS; i++){
            if(!draw_pool[i].active) continue;
            
            if(draw_pool[i].type == DRAW_TYPE_TEXT)
            {
                // 渲染字符串
                int32_t cursor_x = draw_pool[i].data.text_data.x;
                int32_t cursor_y = draw_pool[i].data.text_data.y;
                uint16_t sx = draw_pool[i].data.text_data.sx;
                uint16_t sy = draw_pool[i].data.text_data.sy;
                int32_t scroll = draw_pool[i].data.text_data.scroll_offset;
                
                for(int c=0; c<MAX_STR_LEN; c++){
                    char ch = draw_pool[i].data.text_data.text[c];
                    if(ch == 0) break;
                    if(ch == ' '){
                        cursor_x += (2000 * (int32_t)sx) / 100 + draw_pool[i].data.text_data.spacing;
                        continue;
                    }
                    
                    if(set_pattern_by_char(ch)){
                        int32_t minx, maxx;
                        compute_pattern_minmax_x(current_pattern, current_pattern_length, &minx, &maxx);
                        
                        // 放大字母（单位长度模式 -> DAC 模式）
                        int32_t pre_scale = 1;
                        if((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')){
                            pre_scale = 512;
                        }
                        
                        minx *= pre_scale;
                        maxx *= pre_scale;
                        
                        int32_t char_w = ((maxx - minx) * (int32_t)sx) / 100;
                        int32_t draw_x = cursor_x - scroll;
                        
                        // 可见性检查
                        if(draw_x + char_w < 0) {
                             cursor_x += char_w + draw_pool[i].data.text_data.spacing;
                             continue; 
                        }
                        if(draw_x > 4096) {
                             break; 
                        }

                        // 绘制图案中的每一行
                        for(int l=0; l<current_pattern_length; l++){
                            int32_t x0 = current_pattern[l].x0 * pre_scale;
                            int32_t y0 = current_pattern[l].y0 * pre_scale;
                            int32_t x1 = current_pattern[l].x1 * pre_scale;
                            int32_t y1 = current_pattern[l].y1 * pre_scale;
                            
                            // 变换
                            int32_t tx0 = (x0 * (int32_t)sx) / 100 + cursor_x - (minx * (int32_t)sx) / 100 - scroll;
                            int32_t ty0 = (y0 * (int32_t)sy) / 100 + cursor_y;
                            int32_t tx1 = (x1 * (int32_t)sx) / 100 + cursor_x - (minx * (int32_t)sx) / 100 - scroll;
                            int32_t ty1 = (y1 * (int32_t)sy) / 100 + cursor_y;
                            
                            // 插值线
                            int32_t dx = tx1 - tx0;
                            int32_t dy = ty1 - ty0;
                            
                            // --- 绘图速度 / 密度控制 ---
                            int steps = (int)sqrtf((float)dx*dx + (float)dy*dy) / 5; 
                            
                            if(steps < 2) steps = 2; // 至少起点和终点
                            
                            for(int s=0; s<=steps; s++){
                                if(DAC_Buff_Count >= DRAW_BUF_SIZE) break;
                                int32_t px = tx0 + (dx * s) / steps;
                                int32_t py = ty0 + (dy * s) / steps;
                                
                                // 钳位
                                if(px < 0) px = 0;
                                if(px > 4095) px = 4095;
                                if(py < 0) py = 0;
                                if(py > 4095) py = 4095;
                                
                                DAC_Buff_X[DAC_Buff_Count] = (uint16_t)px;
                                DAC_Buff_Y[DAC_Buff_Count] = (uint16_t)py;
                                
                                DAC_Buff_Count++;
                            }
                        }
                        
                        // 前进光标
                        cursor_x += char_w + draw_pool[i].data.text_data.spacing;
                    }
                }
            }
            else if(draw_pool[i].type == DRAW_TYPE_LINE)
            {
                int32_t x0 = draw_pool[i].data.line_data.x0;
                int32_t y0 = draw_pool[i].data.line_data.y0;
                int32_t x1 = draw_pool[i].data.line_data.x1;
                int32_t y1 = draw_pool[i].data.line_data.y1;
                
                int32_t dx = x1 - x0;
                int32_t dy = y1 - y0;
                int steps = (int)sqrtf((float)dx*dx + (float)dy*dy) / 5;
                if(steps < 2) steps = 2;
                
                for(int s=0; s<=steps; s++){
                    if(DAC_Buff_Count >= DRAW_BUF_SIZE) break;
                    DAC_Buff_X[DAC_Buff_Count] = x0 + (dx * s) / steps;
                    DAC_Buff_Y[DAC_Buff_Count] = y0 + (dy * s) / steps;
                    if(DAC_Buff_X[DAC_Buff_Count] > 4095) DAC_Buff_X[DAC_Buff_Count] = 4095;
                    if(DAC_Buff_Y[DAC_Buff_Count] > 4095) DAC_Buff_Y[DAC_Buff_Count] = 4095;
                    DAC_Buff_Count++;
                }
            }
            else if(draw_pool[i].type == DRAW_TYPE_RECT)
            {
                int32_t x = draw_pool[i].data.rect_data.x;
                int32_t y = draw_pool[i].data.rect_data.y;
                int32_t w = draw_pool[i].data.rect_data.w;
                int32_t h = draw_pool[i].data.rect_data.h;
                
                // 4 条线
                int32_t pts[5][2] = { {x,y}, {x+w,y}, {x+w,y+h}, {x,y+h}, {x,y} };
                
                for(int l=0; l<4; l++){
                    int32_t x0 = pts[l][0];
                    int32_t y0 = pts[l][1];
                    int32_t x1 = pts[l+1][0];
                    int32_t y1 = pts[l+1][1];
                    
                    int32_t dx = x1 - x0;
                    int32_t dy = y1 - y0;
                    int steps = (int)sqrtf((float)dx*dx + (float)dy*dy) / 5;
                    if(steps < 2) steps = 2;
                    
                    for(int s=0; s<=steps; s++){
                        if(DAC_Buff_Count >= DRAW_BUF_SIZE) break;
                        DAC_Buff_X[DAC_Buff_Count] = x0 + (dx * s) / steps;
                        DAC_Buff_Y[DAC_Buff_Count] = y0 + (dy * s) / steps;
                        if(DAC_Buff_X[DAC_Buff_Count] > 4095) DAC_Buff_X[DAC_Buff_Count] = 4095;
                        if(DAC_Buff_Y[DAC_Buff_Count] > 4095) DAC_Buff_Y[DAC_Buff_Count] = 4095;
                        DAC_Buff_Count++;
                    }
                }
            }
            else if(draw_pool[i].type == DRAW_TYPE_CIRCLE)
            {
                int32_t cx = draw_pool[i].data.circle_data.x;
                int32_t cy = draw_pool[i].data.circle_data.y;
                int32_t r = draw_pool[i].data.circle_data.r;
                
                // 周长大约 2*pi*r
                int steps = (int)(6.28 * r) / 5;
                if(steps < 10) steps = 10;
                
                for(int s=0; s<=steps; s++){
                    if(DAC_Buff_Count >= DRAW_BUF_SIZE) break;
                    float angle = (float)s / steps * 6.283185307f;
                    DAC_Buff_X[DAC_Buff_Count] = cx + (int32_t)(cosf(angle) * r);
                    DAC_Buff_Y[DAC_Buff_Count] = cy + (int32_t)(sinf(angle) * r);
                    if(DAC_Buff_X[DAC_Buff_Count] > 4095) DAC_Buff_X[DAC_Buff_Count] = 4095;
                    if(DAC_Buff_Y[DAC_Buff_Count] > 4095) DAC_Buff_Y[DAC_Buff_Count] = 4095;
                    DAC_Buff_Count++;
                }
            }
        }
    }
    
    // 更新 DMA
    if(DAC_Buff_Count > 0){
        DRAW_OUT_STOP_DMA();
        DRAW_OUT_START_DMA(DAC_Buff_X, DAC_Buff_Y, DAC_Buff_Count);
    }
}

void DRAW_Clear(void){
  for(int i=0; i<MAX_DRAW_OBJS; i++) draw_pool[i].active = 0;
  DRAW_Render();
}

void DRAW_Init(uint32_t interval_ms){
  if(patterns_count == 0) return;
  DRAW_Clear();
}

void DRAW_SetScale(uint16_t scale_x_percent, uint16_t scale_y_percent){
  if(scale_x_percent == 0) scale_x_percent = 1;
  if(scale_y_percent == 0) scale_y_percent = 1;
  scale_x_pct = scale_x_percent;
  scale_y_pct = scale_y_percent;
}

void DRAW_SetOffset(int16_t offset_x_param, int16_t offset_y_param){
  offset_x = offset_x_param;
  offset_y = offset_y_param;
}

uint8_t set_pattern_by_char(char c); // 如果需要，前向声明修复，但它是静态的

int16_t DRAW_AddString(const char *s, uint16_t spacing, int32_t x, int32_t y, uint16_t sx, uint16_t sy){
  int slot = -1;
  for(int i=0; i<MAX_DRAW_OBJS; i++){
    if(!draw_pool[i].active){ slot = i; break; }
  }
  if(slot < 0) return -1;

  draw_pool[slot].type = DRAW_TYPE_TEXT;
  strncpy(draw_pool[slot].data.text_data.text, s, MAX_STR_LEN-1);
  draw_pool[slot].data.text_data.text[MAX_STR_LEN-1] = '\0';
  draw_pool[slot].data.text_data.x = x;
  draw_pool[slot].data.text_data.y = y;
  draw_pool[slot].data.text_data.sx = sx;
  draw_pool[slot].data.text_data.sy = sy;
  draw_pool[slot].data.text_data.spacing = spacing;
  
  // 初始化滚动
  draw_pool[slot].data.text_data.scroll_offset = 0;
  draw_pool[slot].data.text_data.last_scroll_time = HAL_GetTick();
  draw_pool[slot].data.text_data.view_width = 4096; // 默认为全屏宽度
  
  // 计算总宽度
  int32_t width = 0;
  for(int c=0; c<MAX_STR_LEN; c++){
      char ch = draw_pool[slot].data.text_data.text[c];
      if(ch == 0) break;
      if(ch == ' '){
          width += (2000 * (int32_t)sx) / 100 + spacing;
          continue;
      }
      if(set_pattern_by_char(ch)){
          int32_t minx, maxx;
          compute_pattern_minmax_x(current_pattern, current_pattern_length, &minx, &maxx);
          int32_t pre_scale = 1;
          if((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')){
              pre_scale = 512;
          }
          minx *= pre_scale;
          maxx *= pre_scale;
          width += ((maxx - minx) * (int32_t)sx) / 100 + spacing;
      }
  }
  draw_pool[slot].data.text_data.total_width = width;

  draw_pool[slot].active = 1;
  
  // 立即更新缓冲区
  // DRAW_Render(); // 为了性能移除（批处理模式）
  
  return (int16_t)slot;
}

int32_t DRAW_GetTextScroll(const char *text) {
    for(int i=0; i<MAX_DRAW_OBJS; i++){
        if(draw_pool[i].active && draw_pool[i].type == DRAW_TYPE_TEXT){
            if(strncmp(draw_pool[i].data.text_data.text, text, MAX_STR_LEN) == 0){
                return draw_pool[i].data.text_data.scroll_offset;
            }
        }
    }
    return 0;
}

void DRAW_SetTextScroll(int16_t slot, int32_t scroll) {
    if(slot >= 0 && slot < MAX_DRAW_OBJS && draw_pool[slot].active && draw_pool[slot].type == DRAW_TYPE_TEXT){
        draw_pool[slot].data.text_data.scroll_offset = scroll;
    }
}

uint8_t DRAW_AddLine(int32_t x0, int32_t y0, int32_t x1, int32_t y1){
  int slot = -1;
  for(int i=0; i<MAX_DRAW_OBJS; i++){
    if(!draw_pool[i].active){ slot = i; break; }
  }
  if(slot < 0) return 0;

  draw_pool[slot].type = DRAW_TYPE_LINE;
  draw_pool[slot].data.line_data.x0 = x0;
  draw_pool[slot].data.line_data.y0 = y0;
  draw_pool[slot].data.line_data.x1 = x1;
  draw_pool[slot].data.line_data.y1 = y1;
  draw_pool[slot].active = 1;
  
  // DRAW_Render();
  return 1;
}

uint8_t DRAW_AddRect(int32_t x, int32_t y, int32_t w, int32_t h){
  int slot = -1;
  for(int i=0; i<MAX_DRAW_OBJS; i++){
    if(!draw_pool[i].active){ slot = i; break; }
  }
  if(slot < 0) return 0;

  draw_pool[slot].type = DRAW_TYPE_RECT;
  draw_pool[slot].data.rect_data.x = x;
  draw_pool[slot].data.rect_data.y = y;
  draw_pool[slot].data.rect_data.w = w;
  draw_pool[slot].data.rect_data.h = h;
  draw_pool[slot].active = 1;
  
  // DRAW_Render();
  return 1;
}

uint8_t DRAW_AddCircle(int32_t x, int32_t y, int32_t r){
  int slot = -1;
  for(int i=0; i<MAX_DRAW_OBJS; i++){
    if(!draw_pool[i].active){ slot = i; break; }
  }
  if(slot < 0) return 0;

  draw_pool[slot].type = DRAW_TYPE_CIRCLE;
  draw_pool[slot].data.circle_data.x = x;
  draw_pool[slot].data.circle_data.y = y;
  draw_pool[slot].data.circle_data.r = r;
  draw_pool[slot].active = 1;
  
  // DRAW_Render();
  return 1;
}

void DRAW_SetLetter(char c){
    char buf[2] = {c, 0};
    DRAW_Clear();
    DRAW_AddString(buf, 0, offset_x, offset_y, scale_x_pct, scale_y_pct);
}

void DRAW_Terminal_Init(uint16_t scale_pct, int32_t spacing){
    if(scale_pct < 1) scale_pct = 1;
    DRAW_Clear();
    term_scale = scale_pct;
    
    term_char_spacing = spacing;
    
    // 估计行高：4096（全高）* scale / 100。
    // 一个字符在图案空间中大约有 4096 个单位高。
    int32_t char_h = (4096 * (int32_t)scale_pct) / 100;
    
    // 添加一些填充（使用字符间距作为垂直填充）
    term_line_height = char_h + (term_char_spacing * 4); 
    
    // 计算最大可见行数
    // 公式：(4096 + padding) / (height + padding)
    term_max_lines = (4096 + term_char_spacing) / term_line_height;
    
    if(term_max_lines > MAX_DRAW_OBJS) term_max_lines = MAX_DRAW_OBJS;
    if(term_max_lines < 1) term_max_lines = 1;
    
    term_current_line = 0;
    // Y 从顶部开始 - line_height（这样第一行是可见的）
    // 等等，如果 Y=0 是底部，那么顶行在 Y=4096 - height。
    term_cursor_y = 4096 - term_line_height;
    term_cursor_x = 0;
}

void DRAW_Terminal_SetSpacing(int32_t spacing){
    term_char_spacing = spacing;
}

void DRAW_Terminal_Print(const char *str){
    // 如果没有活动行，开始一行
    if(term_current_line == 0 && !draw_pool[0].active){
        draw_pool[0].active = 1;
        draw_pool[0].type = DRAW_TYPE_TEXT;
        draw_pool[0].data.text_data.x = 0;
        draw_pool[0].data.text_data.y = term_cursor_y;
        draw_pool[0].data.text_data.sx = term_scale;
        draw_pool[0].data.text_data.sy = term_scale;
        draw_pool[0].data.text_data.spacing = term_char_spacing;
        draw_pool[0].data.text_data.text[0] = '\0';
    }

    int len = strlen(str);
    for(int i=0; i<len; i++){
        char c = str[i];
        
        // 处理换行符
        if(c == '\n'){
            // 移动到下一行
            term_current_line++;
            if(term_current_line >= term_max_lines){
                // 向上滚动
                for(int j=0; j<term_max_lines-1; j++){
                    // 仅当它是文本类型时才复制文本数据
                    if(draw_pool[j+1].type == DRAW_TYPE_TEXT){
                        draw_pool[j].type = DRAW_TYPE_TEXT;
                        strcpy(draw_pool[j].data.text_data.text, draw_pool[j+1].data.text_data.text);
                        draw_pool[j].active = draw_pool[j+1].active;
                    }
                }
                // 清除最后一行
                draw_pool[term_max_lines-1].data.text_data.text[0] = '\0';
                term_current_line = term_max_lines - 1;
            }
            
            // 设置新行
            draw_pool[term_current_line].active = 1;
            draw_pool[term_current_line].type = DRAW_TYPE_TEXT;
            draw_pool[term_current_line].data.text_data.x = 0;
            // 计算此插槽的 Y
            draw_pool[term_current_line].data.text_data.y = 4096 - (term_current_line + 1) * term_line_height;
            draw_pool[term_current_line].data.text_data.sx = term_scale;
            draw_pool[term_current_line].data.text_data.sy = term_scale;
            draw_pool[term_current_line].data.text_data.spacing = term_char_spacing;
            draw_pool[term_current_line].data.text_data.text[0] = '\0';
            
            term_cursor_x = 0;
            continue;
        }
        
        // 检查宽度（粗略估计）
        // 字符宽度大约 2000 个单位（未缩放）
        int32_t char_w = (2000 * (int32_t)term_scale) / 100 + term_char_spacing;
        if(term_cursor_x + char_w > 4096){
             // 自动换行
             term_current_line++;
             if(term_current_line >= term_max_lines){
                for(int j=0; j<term_max_lines-1; j++){
                    if(draw_pool[j+1].type == DRAW_TYPE_TEXT){
                        draw_pool[j].type = DRAW_TYPE_TEXT;
                        strcpy(draw_pool[j].data.text_data.text, draw_pool[j+1].data.text_data.text);
                        draw_pool[j].active = draw_pool[j+1].active;
                    }
                }
                draw_pool[term_max_lines-1].data.text_data.text[0] = '\0';
                term_current_line = term_max_lines - 1;
             }
             draw_pool[term_current_line].active = 1;
             draw_pool[term_current_line].type = DRAW_TYPE_TEXT;
             draw_pool[term_current_line].data.text_data.x = 0;
             draw_pool[term_current_line].data.text_data.y = 4096 - (term_current_line + 1) * term_line_height;
             draw_pool[term_current_line].data.text_data.sx = term_scale;
             draw_pool[term_current_line].data.text_data.sy = term_scale;
             draw_pool[term_current_line].data.text_data.spacing = term_char_spacing;
             draw_pool[term_current_line].data.text_data.text[0] = '\0';
             term_cursor_x = 0;
        }
        
        // 追加字符
        int cur_len = strlen(draw_pool[term_current_line].data.text_data.text);
        if(cur_len < MAX_STR_LEN - 1){
            draw_pool[term_current_line].data.text_data.text[cur_len] = c;
            draw_pool[term_current_line].data.text_data.text[cur_len+1] = '\0';
            term_cursor_x += char_w;
        }
    }
    
    DRAW_Render();
}

// End of file
