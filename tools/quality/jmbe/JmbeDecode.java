// SPDX-License-Identifier: GPL-3.0-or-later
// Decode a file of 18-byte IMBE 7200x4400 frames (P25 Phase 1 layout, MSB first: the
// output of `mbe-cli encode --rate imbe7200`) with JMBE (sdrtrunk's Java MBE decoder) into
// a 16-bit 8 kHz mono wav, for the cross-check in docs/encoder-quality.md.
//   javac -cp jmbe-1.0.9.jar;jmbe-api-1.0.9.jar JmbeDecode.java
//   java -cp .;jmbe-1.0.9.jar;jmbe-api-1.0.9.jar;<slf4j, JTransforms, JLargeArrays jars> JmbeDecode in.dvsi out.wav [parms.txt]
// The optional parms.txt gets one line per frame: w0 L voicing(0/1 string) M[1..L] (the
// spectral amplitudes before enhancement), the format of `mbe-cli parms --rate imbe7200`.
import jmbe.codec.imbe.IMBEFrame;
import jmbe.codec.imbe.IMBEModelParameters;
import jmbe.codec.imbe.IMBESynthesizer;

import java.io.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Paths;

public class JmbeDecode {
    public static void main(String[] args) throws IOException {
        byte[] data = Files.readAllBytes(Paths.get(args[0]));
        int frames = data.length / 18;
        IMBESynthesizer synth = new IMBESynthesizer();
        ByteBuffer pcm = ByteBuffer.allocate(frames * 320).order(ByteOrder.LITTLE_ENDIAN);
        PrintWriter parms = args.length > 2 ? new PrintWriter(new FileWriter(args[2])) : null;
        for (int i = 0; i < frames; i++) {
            byte[] frame = new byte[18];
            System.arraycopy(data, i * 18, frame, 0, 18);
            float[] audio = synth.getAudio(new IMBEFrame(frame));
            if (parms != null) {
                IMBEModelParameters p = (IMBEModelParameters) synth.getPreviousFrame();
                StringBuilder sb = new StringBuilder();
                sb.append(p.getRepeatCount()).append(' ').append(p.getFundamentalFrequency()).append(' ').append(p.getL()).append(' ');
                boolean[] v = p.getVoicingDecisions();
                for (int l = 1; l <= p.getL(); l++) sb.append(l < v.length && v[l] ? '1' : '0');
                float[] m = p.getSpectralAmplitudes();
                for (int l = 1; l <= p.getL(); l++) sb.append(' ').append(l < m.length ? m[l] : 0.0f);
                parms.println(sb);
            }
            for (int n = 0; n < 160; n++) {
                float v = n < audio.length ? audio[n] : 0.0f;
                int s = Math.round(v * 32767.0f);
                pcm.putShort((short) Math.max(-32768, Math.min(32767, s)));
            }
        }
        try (DataOutputStream out = new DataOutputStream(new BufferedOutputStream(new FileOutputStream(args[1])))) {
            int dataLen = frames * 320;
            ByteBuffer h = ByteBuffer.allocate(44).order(ByteOrder.LITTLE_ENDIAN);
            h.put("RIFF".getBytes()).putInt(36 + dataLen).put("WAVE".getBytes());
            h.put("fmt ".getBytes()).putInt(16).putShort((short) 1).putShort((short) 1).putInt(8000).putInt(16000).putShort((short) 2).putShort((short) 16);
            h.put("data".getBytes()).putInt(dataLen);
            out.write(h.array());
            out.write(pcm.array(), 0, dataLen);
        }
        if (parms != null) parms.close();
        System.err.println("decoded " + frames + " frames");
    }
}
