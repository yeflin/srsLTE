/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2014 The libLTE Developers. See the
 * COPYRIGHT file at the top-level directory of this distribution.
 *
 * \section LICENSE
 *
 * This file is part of the libLTE library.
 *
 * libLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * libLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * A copy of the GNU Lesser General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <complex.h>
#include <math.h>

#include "srslte/phy/common/phy_common.h"
#include "srslte/phy/utils/dft.h"
#include "srslte/phy/common/fft.h"
#include "srslte/phy/utils/debug.h"
#include "srslte/phy/utils/vector.h"

int lte_fft_init_(lte_fft_t *q, lte_cp_t cp, uint32_t nof_prb, dft_dir_t dir) {
  int symbol_sz = lte_symbol_sz(nof_prb);

  if (symbol_sz < 0) {
    fprintf(stderr, "Error: Invalid nof_prb=%d\n", nof_prb);
    return -1;
  }
  if (dft_plan_c(&q->fft_plan, symbol_sz, dir)) {
    fprintf(stderr, "Error: Creating DFT plan\n");
    return -1;
  }
  q->tmp = malloc((uint32_t) symbol_sz * sizeof(cf_t));
  if (!q->tmp) {
    perror("malloc");
    return -1;
  }

  dft_plan_set_mirror(&q->fft_plan, true);
  dft_plan_set_dc(&q->fft_plan, true);

  q->symbol_sz = (uint32_t) symbol_sz;
  q->nof_symbols = CP_NSYMB(cp);
  q->cp = cp;
  q->freq_shift = false;
  q->nof_re = nof_prb * RE_X_RB;
  q->nof_guards = ((symbol_sz - q->nof_re) / 2);
  q->slot_sz = SLOT_LEN(symbol_sz);
  
  DEBUG("Init %s symbol_sz=%d, nof_symbols=%d, cp=%s, nof_re=%d, nof_guards=%d\n",
      dir==FORWARD?"FFT":"iFFT", q->symbol_sz, q->nof_symbols,
          q->cp==CPNORM?"Normal":"Extended", q->nof_re, q->nof_guards);

  return LIBLTE_SUCCESS;
}

void lte_fft_free_(lte_fft_t *q) {
  dft_plan_free(&q->fft_plan);
  if (q->tmp) {
    free(q->tmp);
  }
  if (q->shift_buffer) {
    free(q->shift_buffer);
  }
  bzero(q, sizeof(lte_fft_t));
}

int lte_fft_init(lte_fft_t *q, lte_cp_t cp, uint32_t nof_prb) {
  return lte_fft_init_(q, cp, nof_prb, FORWARD);
}

void lte_fft_free(lte_fft_t *q) {
  lte_fft_free_(q);
}

int lte_ifft_init(lte_fft_t *q, lte_cp_t cp, uint32_t nof_prb) {
  uint32_t i;
  int ret;
  
  ret = lte_fft_init_(q, cp, nof_prb, BACKWARD); 
  
  if (ret == LIBLTE_SUCCESS) {
    dft_plan_set_norm(&q->fft_plan, true);
    
    /* set now zeros at CP */
    for (i=0;i<q->nof_symbols;i++) {
      bzero(q->tmp, q->nof_guards * sizeof(cf_t));
      bzero(&q->tmp[q->nof_re + q->nof_guards], q->nof_guards * sizeof(cf_t));
    }
  }
  return ret;
}

/* Shifts the signal after the iFFT or before the FFT. 
 * Freq_shift is relative to inter-carrier spacing.
 * Caution: This function shall not be called during run-time 
 */
int lte_fft_set_freq_shift(lte_fft_t *q, float freq_shift) {
  q->shift_buffer = vec_malloc(sizeof(cf_t) * SF_LEN(q->symbol_sz));
  if (!q->shift_buffer) {
    perror("malloc");
    return -1; 
  }
  cf_t *ptr = q->shift_buffer;
  for (uint32_t n=0;n<2;n++) {
    for (uint32_t i=0;i<q->nof_symbols;i++) {
      uint32_t cplen = CP_ISNORM(q->cp)?CP_NORM(i, q->symbol_sz):CP_EXT(q->symbol_sz);
      for (uint32_t t=0;t<q->symbol_sz+cplen;t++) {
        ptr[t] = cexpf(I*2*M_PI*((float) t-(float)cplen)*freq_shift/q->symbol_sz);
      }
      ptr += q->symbol_sz+cplen;
    }    
  }
  
  /* Disable DC carrier addition */
  dft_plan_set_dc(&q->fft_plan, false);  
 
  q->freq_shift = true;
  
  return LIBLTE_SUCCESS;
}

void lte_ifft_free(lte_fft_t *q) {
  lte_fft_free_(q);
}

/* Transforms input samples into output OFDM symbols.
 * Performs FFT on a each symbol and removes CP.
 */
void lte_fft_run_slot(lte_fft_t *q, cf_t *input, cf_t *output) {
  uint32_t i;
  for (i=0;i<q->nof_symbols;i++) {
    input += CP_ISNORM(q->cp)?CP_NORM(i, q->symbol_sz):CP_EXT(q->symbol_sz);
    dft_run_c(&q->fft_plan, input, q->tmp);
    memcpy(output, &q->tmp[q->nof_guards], q->nof_re * sizeof(cf_t));
    input += q->symbol_sz;
    output += q->nof_re;
  }
}

void lte_fft_run_sf(lte_fft_t *q, cf_t *input, cf_t *output) {
  uint32_t n; 
  if (q->freq_shift) {
    vec_prod_ccc(input, q->shift_buffer, input, 2*q->slot_sz);
  }
  for (n=0;n<2;n++) {
    lte_fft_run_slot(q, &input[n*q->slot_sz], &output[n*q->nof_re*q->nof_symbols]);
  }
}

/* Transforms input OFDM symbols into output samples.
 * Performs FFT on a each symbol and adds CP.
 */
void lte_ifft_run_slot(lte_fft_t *q, cf_t *input, cf_t *output) {
  uint32_t i, cp_len;
  for (i=0;i<q->nof_symbols;i++) {
    cp_len = CP_ISNORM(q->cp)?CP_NORM(i, q->symbol_sz):CP_EXT(q->symbol_sz);
    memcpy(&q->tmp[q->nof_guards], input, q->nof_re * sizeof(cf_t));
    dft_run_c(&q->fft_plan, q->tmp, &output[cp_len]);
    input += q->nof_re;
    /* add CP */
    memcpy(output, &output[q->symbol_sz], cp_len * sizeof(cf_t));
    output += q->symbol_sz + cp_len;
  }
}

void lte_fft_set_normalize(lte_fft_t *q, bool normalize_enable) {
  dft_plan_set_norm(&q->fft_plan, normalize_enable);
}

void lte_ifft_run_sf(lte_fft_t *q, cf_t *input, cf_t *output) {
  uint32_t n; 
  for (n=0;n<2;n++) {
    lte_ifft_run_slot(q, &input[n*q->nof_re*q->nof_symbols], &output[n*q->slot_sz]);
  }
  if (q->freq_shift) {
    vec_prod_ccc(output, q->shift_buffer, output, 2*q->slot_sz);
  }
}