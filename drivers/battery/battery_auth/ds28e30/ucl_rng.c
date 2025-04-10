/*******************************************************************************
 * Copyright (C) 2015 Maxim Integrated Products, Inc., All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY,  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL MAXIM INTEGRATED BE LIABLE FOR ANY CLAIM, DAMAGES
 * OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of Maxim Integrated shall
 * not be used except as stated in the Maxim Integrated Products, Inc. Branding Policy.
 *
 * The mere transfer of this software does not imply any licenses
 * of trade secrets, proprietary technology, copyrights, patents,
 * trademarks, maskwork rights, or any other form of intellectual
 * property whatsoever. Maxim Integrated Products, Inc. retains all ownership rights.
 *******************************************************************************/

#include "ucl_config.h"
#include "ucl_types.h"
#include "ucl_sys.h"
#include "ucl_defs.h"
#include "ucl_retdefs.h"
#include "ucl_rng.h"
#include "ucl_hash.h"
#ifdef HASH_SHA256
#include "ucl_sha256.h"
#endif


//this is not secure for ECDSA signatures, as being pseudo random number generator
//this is for test and demo only
int ucl_rng_read(u8 *rand,u32 rand_byteLen)
{
  int msgi,j;
  static u8 pseudo[16]={0x11,0x22,0x33,0x44,0x55,0x00,0x11,0x22,0x33,0x44,0x55,0x00,0x11,0x22,0x33,0x44};
  u8 output[32],input[16];
  u8 blocksize;
  blocksize=16;
      
  
  for(msgi=0;msgi<(int)rand_byteLen;)
    {
      for(j=0;j<blocksize;j++)
	input[j]=pseudo[j];
      ucl_sha256(output,input,blocksize);
      for(j=0;j<blocksize;j++)
	pseudo[j]=output[j];
      for(j=0;j<blocksize;j++)
	{
	  if(msgi<(int)rand_byteLen)
	    {
	      rand[msgi]=output[j];
	      msgi++;
	    }
	}
    }
  return(rand_byteLen);
}
