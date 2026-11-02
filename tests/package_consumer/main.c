/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <mbelib/mbelib.h>
#include <mbelib-neo/mbelib.h>
#include <mbevocoder/mbevocoder.h>

int main(void)
{
    short pcm[160] = {0};
    char bits[49];
    mbe_encoder* encoder = mbe_encoderCreate(MBE_RATE_AMBE2450, NULL);
    int status;
    if (!encoder) return 1;
    status = mbe_encoderEncodeParms(encoder, pcm, bits, NULL);
    mbe_encoderDestroy(encoder);
    return status != 0;
}
