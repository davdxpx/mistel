package io.github.davdxpx.mistel.mncf;

import java.io.ByteArrayOutputStream;
import java.nio.charset.StandardCharsets;
import java.util.zip.CRC32;

/** Test data shared by the unit tests: a small file, RFC 8032 vectors and hand-made ZIP files. */
final class Fixtures {

    private Fixtures() {
    }

    /** RFC 8032, section 7.1: secret key, public key, message, signature. */
    static final String[][] RFC8032 = {
        {"9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b"},
        {"4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00"},
        {"c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a"},
        {"833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
            "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
            "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704"},
    };

    /** The public key of the first RFC 8032 vector, which signed the lines below. */
    static final byte[] PUBLIC_KEY = hex(RFC8032[0][1]);

    static final String FILE_TEXT = "format = \"mncf/1\"\n"
            + "name = \"Example\"\n"
            + "\n"
            + "[game]\n"
            + "platform = \"switch\"\n"
            + "id = \"0100000000ABC000\"\n"
            + "\n"
            + "[[cheat]]\n"
            + "id = \"money\"\n"
            + "name = \"Money\"\n"
            + "\n"
            + "  [[cheat.write]]\n"
            + "  at = { region = \"heap\", offset = 0x10 }\n"
            + "  type = \"u32\"\n"
            + "  value = 5\n";

    static final byte[] FILE = utf8(FILE_TEXT);

    /** FILE's signature line by the first RFC 8032 key, made with the reference reader's signing tool. */
    static final String FILE_SIGNATURE = "# mncf-signature: ed25519 21fe31dfa154a261 "
            + "TxX03pZH0h9+vg9B1Dks1hU9In9Nt9W79P0QPchoHij77i8jbmIbbZA7gXUAlMxB1xGh+b4LY9PjND4ynTBuDw==\n";

    /** The SHA-256 hash of FILE. */
    static final String FILE_SHA256 = "793bf91e97b0ed6956ed6e17b8f9e6967a7a824dc820d4e580d41f5792964df8";

    static final String MANIFEST_TEXT = "format = \"mncf-pack/1\"\nname = \"P\"\n\n[[file]]\npath = \"a.mncf\"\nsha256 = \""
            + FILE_SHA256 + "\"\n";

    /** MANIFEST_TEXT's signature line by the first RFC 8032 key. */
    static final String MANIFEST_SIGNATURE = "# mncf-signature: ed25519 21fe31dfa154a261 "
            + "uS/DCk+GRwGp7ImYJ3ufGYSOdtoSn4xgRQNaZGXspnmtq5ZOTY+/HPtk+gnbtq8pWndeZUolz6VPV3MSJJPxDg==\n";

    static byte[] utf8(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    static byte[] hex(String s) {
        return ConformanceTest.hex(s);
    }

    static byte[] concat(byte[]... parts) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        for (byte[] p : parts) {
            out.write(p, 0, p.length);
        }
        return out.toByteArray();
    }

    static TrustList trustFirstKey() {
        return new TrustList().trust("one", PUBLIC_KEY);
    }

    /**
     * A ZIP file of stored entries, written by hand so that a test can also make what a ZIP
     * library refuses to write, such as two entries of the same name.
     *
     * @param namesAndData entry names and their bytes, alternating
     */
    static byte[] storedZip(Object... namesAndData) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        ByteArrayOutputStream central = new ByteArrayOutputStream();
        int count = namesAndData.length / 2;
        for (int i = 0; i < count; i++) {
            byte[] name = utf8((String) namesAndData[2 * i]);
            byte[] data = (byte[]) namesAndData[2 * i + 1];
            CRC32 crc = new CRC32();
            crc.update(data);
            int offset = out.size();
            le(out, 0x04034b50, 4);
            le(out, 20, 2);
            le(out, 0, 2);
            le(out, 0, 2);
            le(out, 0, 4);
            le(out, crc.getValue(), 4);
            le(out, data.length, 4);
            le(out, data.length, 4);
            le(out, name.length, 2);
            le(out, 0, 2);
            out.write(name, 0, name.length);
            out.write(data, 0, data.length);
            le(central, 0x02014b50, 4);
            le(central, 20, 2);
            le(central, 20, 2);
            le(central, 0, 2);
            le(central, 0, 2);
            le(central, 0, 4);
            le(central, crc.getValue(), 4);
            le(central, data.length, 4);
            le(central, data.length, 4);
            le(central, name.length, 2);
            le(central, 0, 2);
            le(central, 0, 2);
            le(central, 0, 2);
            le(central, 0, 2);
            le(central, 0, 4);
            le(central, offset, 4);
            central.write(name, 0, name.length);
        }
        int centralAt = out.size();
        byte[] cd = central.toByteArray();
        out.write(cd, 0, cd.length);
        le(out, 0x06054b50, 4);
        le(out, 0, 2);
        le(out, 0, 2);
        le(out, count, 2);
        le(out, count, 2);
        le(out, cd.length, 4);
        le(out, centralAt, 4);
        le(out, 0, 2);
        return out.toByteArray();
    }

    private static void le(ByteArrayOutputStream out, long v, int size) {
        for (int i = 0; i < size; i++) {
            out.write((int) (v >>> (8 * i)) & 0xFF);
        }
    }
}
