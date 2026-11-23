package io.github.iamwilmar.mhriseport;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.security.GeneralSecurityException;
import java.security.SecureRandom;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.DataFormatException;
import java.util.zip.Deflater;
import java.util.zip.Inflater;

import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

// MH Rise's save files, Switch and Steam, read and written without Android (SaveFiles does the zips, folders and the
// menu's side). Same file names on both: data001Slot.bin (the hunters) and data00-1.bin (options, system data).
//
// Each one is a DSSS v2 container around the game's own serialized data, and ends with the murmur3 (seed ~0) of the
// rest. What changes between the two:
//   Switch  flags 8: the data deflated (raw), after a header with its first word, the sizes and a 1.
//   Steam   flags 4 ("Citrus"): the data encrypted with a key made from your Steam ID (below). SS1_* files: flags 2,
//           your Steam account's number before the data; the game makes those again, so they aren't converted.
//   Either  flags 0: plain, what a save editor writes once the encryption is taken off.
//
// The data is a list of classes (a hash of the name, then fields: a hash, a type, a value), and the game aligns each
// value counting from the start of the FILE, not of the data: on Switch the data starts at 12, on Steam at 16 (24 in the
// SS1 files), so the same save has different padding on each and has to be laid out again, not just copied. The two
// versions also don't have exactly the same fields, so converting fills a save of the other platform: its classes and
// fields in its order, with the values from the one being converted wherever the field (hash and type) is in both.
//
// Format from kvasszn's ree-save-editor (GPL-3.0), checked against real saves from both platforms; the Citrus curves
// (assets/citrus_curves.bin) are the game's own constants as that project lists them. No code from it here.
final class SaveData {
    private SaveData() {}

    static final String SLOT = "data001Slot.bin", SYSTEM = "data00-1.bin";
    // The three character slots each have their own file (a save with its hunter in slot 2 has only data002Slot.bin)
    static final String[] SLOTS = {SLOT, "data002Slot.bin", "data003Slot.bin"};

    static boolean slot(String name) {
        for (String s : SLOTS)
            if (s.equals(name)) return true;
        return false;
    }
    static final int FLAG_HAS_ID = 0x2, FLAG_CITRUS = 0x4, FLAG_DEFLATE = 0x8;
    static final int SWITCH_BASE = 12, STEAM_BASE = 16;

    static class Problem extends IOException {
        Problem(String m) {
            super(m);
        }
    }

    // ---- the data's tree ------------------------------------------------------------------------------------------------

    static final class Cls {
        int hash;
        List<Field> fields = new ArrayList<>();
    }

    static final class Field {
        int hash, type;
        Object value;  // Cls (0x11), Arr (-1), Str (0xf), Val (the rest)

        Field(int hash, int type, Object value) {
            this.hash = hash;
            this.type = type;
            this.value = value;
        }
    }

    static final class Arr {
        int memberType, memberSize, kind;  // kind 1: an array of classes
        int[] hashes;                      // kind 1 only, and only if the file had them (after 0xffeeffee)
        List<Object> items = new ArrayList<>();  // Cls, or byte[] (a value, or a string's UTF-16)
    }

    static final class Str {
        byte[] utf16;

        Str(byte[] b) {
            utf16 = b;
        }
    }

    static final class Val {
        byte[] data;

        Val(byte[] b) {
            data = b;
        }
    }

    static final class Root {
        int hash;
        Cls cls;

        Root(int hash, Cls cls) {
            this.hash = hash;
            this.cls = cls;
        }
    }

    // The game's rounding, counted from the file's start: (pos + a - 1) & ~(a - 1), even when a isn't a power of two
    // (a 12-byte struct ends up on a multiple of 4 with bit 3 clear). Odd, but it's what the files have.
    private static int align(int pos, int a, int base) {
        return a > 1 ? ((pos + base + a - 1) & ~(a - 1)) - base : pos;
    }

    private static final class Reader {
        final byte[] b;
        final int base;
        int p;

        Reader(byte[] b, int base) {
            this.b = b;
            this.base = base;
        }

        void al(int a) {
            p = align(p, a, base);
        }

        int u32() throws Problem {
            need(4);
            int v = (b[p] & 0xff) | (b[p + 1] & 0xff) << 8 | (b[p + 2] & 0xff) << 16 | (b[p + 3] & 0xff) << 24;
            p += 4;
            return v;
        }

        void need(long n) throws Problem {
            if (n < 0 || p + n > b.length) throw new Problem("the data ends early");
        }

        byte[] bytes(long n) throws Problem {
            need(n);
            byte[] out = Arrays.copyOfRange(b, p, p + (int) n);
            p += (int) n;
            return out;
        }

        Cls cls() throws Problem {
            int n = u32();
            Cls c = new Cls();
            c.hash = u32();
            if (n < 0 || n > 65536) throw new Problem("not a class");
            for (int i = 0; i < n; i++) {
                int h = u32(), t = u32();
                Field f = new Field(h, t, value(t));
                al(4);
                c.fields.add(f);
            }
            return c;
        }

        Object value(int t) throws Problem {
            if (t == -1) {
                al(4);
                Arr a = new Arr();
                a.memberType = u32();
                a.memberSize = u32();
                long len = u32() & 0xffffffffL;
                a.kind = u32();
                if (len > b.length - p) throw new Problem("an array longer than the file");
                if (a.kind == 1) {
                    if (p + 4 <= b.length && u32() == 0xffeeffee) {
                        a.hashes = new int[(int) len];
                        for (int i = 0; i < len; i++) a.hashes[i] = u32();
                    } else {
                        p -= 4;
                    }
                }
                for (long i = 0; i < len; i++) {
                    if (a.kind == 1) {
                        a.items.add(cls());
                    } else if (a.memberType == 0xf) {
                        al(4);
                        a.items.add(bytes(2L * (u32() & 0xffffffffL)));
                    } else {
                        al(a.memberSize);
                        a.items.add(bytes(a.memberSize & 0xffffffffL));
                    }
                }
                al(4);
                return a;
            }
            if (t == 0x11) return cls();
            if (t == 0xf) {
                al(4);
                return new Str(bytes(2L * (u32() & 0xffffffffL)));
            }
            if (t == 0) throw new Problem("a field of type 0");
            al(4);
            int size = u32();
            if (size < 0 || size > b.length) throw new Problem("a value larger than the file");
            al(size);
            return new Val(bytes(size));
        }
    }

    // The data as its roots. Throws if it doesn't read to the end with this base (a Switch file read as Steam's, say).
    static List<Root> parse(byte[] body, int base) throws Problem {
        Reader r = new Reader(body, base);
        List<Root> out = new ArrayList<>();
        try {
            while (r.p + 12 <= body.length) {
                int h = r.u32();
                out.add(new Root(h, r.cls()));
            }
        } catch (RuntimeException e) {  // (garbage read as sizes)
            throw new Problem("the data can't be read");
        }
        if (out.isEmpty()) throw new Problem("no data in it");
        return out;
    }

    private static final class Writer {
        final ByteArrayOutputStream o = new ByteArrayOutputStream();
        final int base;

        Writer(int base) {
            this.base = base;
        }

        void al(int a) {
            for (int q = align(o.size(), a, base); o.size() < q; ) o.write(0);
        }

        void u32(int v) {
            o.write(v);
            o.write(v >>> 8);
            o.write(v >>> 16);
            o.write(v >>> 24);
        }

        void raw(byte[] b) {
            o.write(b, 0, b.length);
        }

        void cls(Cls c) {
            u32(c.fields.size());
            u32(c.hash);
            for (Field f : c.fields) {
                u32(f.hash);
                u32(f.type);
                value(f.value);
                al(4);
            }
        }

        void value(Object v) {
            if (v instanceof Arr) {
                Arr a = (Arr) v;
                al(4);
                u32(a.memberType);
                u32(a.memberSize);
                u32(a.items.size());
                u32(a.kind);
                if (a.kind == 1 && a.hashes != null) {
                    u32(0xffeeffee);
                    for (Object e : a.items) u32(((Cls) e).hash);
                }
                for (Object e : a.items) {
                    if (a.kind == 1) {
                        cls((Cls) e);
                    } else if (a.memberType == 0xf) {
                        al(4);
                        u32(((byte[]) e).length / 2);
                        raw((byte[]) e);
                    } else {
                        al(a.memberSize);
                        raw((byte[]) e);
                    }
                }
                al(4);
            } else if (v instanceof Cls) {
                cls((Cls) v);
            } else if (v instanceof Str) {
                al(4);
                u32(((Str) v).utf16.length / 2);
                raw(((Str) v).utf16);
            } else {
                byte[] d = ((Val) v).data;
                al(4);
                u32(d.length);
                al(d.length);
                raw(d);
            }
        }
    }

    static byte[] write(List<Root> roots, int base) {
        Writer w = new Writer(base);
        for (Root r : roots) {
            w.u32(r.hash);
            w.cls(r.cls);
        }
        return w.o.toByteArray();
    }

    // ---- one platform's save filled from the other's --------------------------------------------------------------------

    // Roots only Steam's data has, left out when there's no Switch save to take the layout from
    private static final int[] STEAM_ONLY_SLOT = {0xbb431b76, 0x5a04dbf9};
    private static final int[] STEAM_ONLY_SYSTEM = {0x649e38ff, 0x5a04dbf9, 0x819571e6};

    static final class Counts {
        int taken, kept, dropped;
    }

    // The template's roots, classes and fields, with the source's values where it has the same field. Without a template,
    // the source as it is (less what only Steam has, when going to the Switch).
    static List<Root> fill(List<Root> template, List<Root> source, String name, boolean toSwitch, Counts n) {
        if (template == null) {
            List<Root> out = new ArrayList<>();
            int[] skip = toSwitch ? (name.equals(SYSTEM) ? STEAM_ONLY_SYSTEM : STEAM_ONLY_SLOT) : new int[0];
            for (Root r : source) {
                boolean drop = false;
                for (int h : skip) drop |= r.hash == h;
                if (drop) n.dropped++;
                else out.add(r);
            }
            return out;
        }
        Map<Integer, Cls> from = new HashMap<>();
        for (Root r : source) from.putIfAbsent(r.hash, r.cls);
        List<Root> out = new ArrayList<>();
        for (Root r : template) {
            Cls src = from.get(r.hash);
            // Each root has its own revision number: one the other save wrote at another revision isn't read the same way
            // (the Netflix release's b0ca70c9 is at 22, 1.16's at 14, and the game crashed loading it into the world), so
            // the template's stays. Switch and Steam have the same revisions.
            if (src != null && revision(src) != revision(r.cls)) {
                n.kept++;
                out.add(r);
                continue;
            }
            out.add(new Root(r.hash, fillClass(r.cls, src, n)));
        }
        for (Root r : source)
            if (!containsRoot(template, r.hash)) n.dropped++;
        return out;
    }

    private static final int REVISION = 0xef10b158;

    // A root's revision (its field 0xef10b158); -1 if it has none
    private static long revision(Cls c) {
        for (Field f : c.fields)
            if (f.hash == REVISION && f.value instanceof Val && ((Val) f.value).data.length == 4) return le32(((Val) f.value).data, 0);
        return -1;
    }

    private static boolean containsRoot(List<Root> roots, int hash) {
        for (Root r : roots)
            if (r.hash == hash) return true;
        return false;
    }

    private static Cls fillClass(Cls into, Cls from, Counts n) {
        if (from == null || from.hash != into.hash) return into;
        Map<Integer, Field> fields = new HashMap<>();
        for (Field f : from.fields) fields.put(f.hash, f);
        Cls out = new Cls();
        out.hash = into.hash;
        for (Field f : into.fields) {
            Field s = fields.remove(f.hash);
            if (s != null && s.type == f.type) {
                out.fields.add(new Field(f.hash, f.type, fillValue(f.value, s.value, n)));
            } else {
                n.kept++;
                out.fields.add(f);
            }
        }
        n.dropped += fields.size();
        return out;
    }

    private static Object fillValue(Object into, Object from, Counts n) {
        if (into.getClass() != from.getClass()) {
            n.kept++;
            return into;
        }
        if (into instanceof Cls) return fillClass((Cls) into, (Cls) from, n);
        if (into instanceof Arr) {
            Arr a = (Arr) into, s = (Arr) from;
            if (a.memberType != s.memberType || a.memberSize != s.memberSize || a.kind != s.kind) {
                n.kept++;
                return into;
            }
            n.taken++;
            if (a.kind != 1) return from;
            // At least as many elements as the source has (the lengths change with what the save has);
            // the ones the template has no counterpart for keep their own layout, or take the first element's if it's
            // the same class
            Arr out = new Arr();
            out.memberType = a.memberType;
            out.memberSize = a.memberSize;
            out.kind = 1;
            Cls first = a.items.isEmpty() ? null : (Cls) a.items.get(0);
            for (int i = 0; i < s.items.size(); i++) {
                Cls e = (Cls) s.items.get(i);
                if (i < a.items.size()) out.items.add(fillClass((Cls) a.items.get(i), e, n));
                else out.items.add(first != null && first.hash == e.hash ? fillClass(first, e, n) : e);
            }
            // Fewer than the template has: the rest from the template. Some of these are fixed slots, and a save from
            // another release can have fewer of them (the Netflix one: 6 of 1.16's 8 in the hunter's section, and the
            // game crashed loading into the world)
            for (int i = s.items.size(); i < a.items.size(); i++) {
                out.items.add(a.items.get(i));
                n.kept++;
            }
            if (a.hashes != null || s.hashes != null) out.hashes = new int[out.items.size()];
            return out;
        }
        if (into instanceof Val && ((Val) into).data.length != ((Val) from).data.length) {
            n.kept++;
            return into;
        }
        n.taken++;
        return from;
    }

    // ---- containers -----------------------------------------------------------------------------------------------------

    static final class Opened {
        String name;
        int flags;
        boolean steam;     // laid out for Steam (encrypted, with an account ID, or plain that only reads that way)
        int base;          // where its data starts in its file, for the alignment
        byte[] body;
        List<Root> roots;
        int curve = -1;    // Citrus: the curve the Steam ID's key is on (needed to encrypt again)
        long steamId;      // Citrus: the ID that opened it
        boolean foreign;   // neither Switch's nor Steam's: plain main files from another release (converted too)
    }

    static int murmur3(byte[] d, int len) {
        final int c1 = 0xcc9e2d51, c2 = 0x1b873593;
        int h = 0xffffffff, n = len / 4;
        for (int i = 0; i < n; i++) {
            int k = le32(d, i * 4) * c1;
            k = Integer.rotateLeft(k, 15) * c2;
            h ^= k;
            h = Integer.rotateLeft(h, 13) * 5 + 0xe6546b64;
        }
        int k = 0;
        for (int i = len - 1; i >= n * 4; i--) k = (k << 8) | (d[i] & 0xff);
        if (len % 4 != 0) {
            k = Integer.rotateLeft(k * c1, 15) * c2;
            h ^= k;
        }
        h ^= len;
        h ^= h >>> 16;
        h *= 0x85ebca6b;
        h ^= h >>> 13;
        h *= 0xc2b2ae35;
        return h ^ (h >>> 16);
    }

    static int le32(byte[] d, int at) {
        return (d[at] & 0xff) | (d[at + 1] & 0xff) << 8 | (d[at + 2] & 0xff) << 16 | (d[at + 3] & 0xff) << 24;
    }

    static long le64(byte[] d, int at) {
        return (le32(d, at) & 0xffffffffL) | (long) le32(d, at + 4) << 32;
    }

    private static ByteBuffer le(int size) {
        return ByteBuffer.allocate(size).order(ByteOrder.LITTLE_ENDIAN);
    }

    private static byte[] withHash(byte[] head, byte[] rest) {
        int len = (head.length + rest.length + 3) & ~3;
        byte[] out = Arrays.copyOf(head, len + 4);
        System.arraycopy(rest, 0, out, head.length, rest.length);
        ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN).putInt(len, murmur3(out, len));
        return out;
    }

    // What a file has. steamIds: the IDs to try for an encrypted one (the first whose key opens it); Citrus.NEED_ID is
    // thrown when there are none or none fit.
    static Opened open(String name, byte[] d, long[] steamIds, Citrus citrus, int curveHint) throws Problem {
        if (d.length < 20 || d[0] != 'D' || d[1] != 'S' || d[2] != 'S' || d[3] != 'S' || le32(d, 4) != 2)
            throw new Problem(name + " isn't an MH Rise save");
        Opened o = new Opened();
        o.name = name;
        o.flags = le32(d, 8);
        int end = d.length - 4;
        if (o.flags == 0) {
            o.body = Arrays.copyOfRange(d, 12, end);
        } else if (o.flags == FLAG_HAS_ID) {
            o.body = Arrays.copyOfRange(d, 0x18, end);
            o.steam = true;
        } else if (o.flags == FLAG_DEFLATE) {
            int csize = le32(d, 0x1c);
            long size = le64(d, 0x20);
            if (csize < 0 || 0x28L + csize > end || size < 0 || size > 1 << 28) throw new Problem(name + ": its sizes are wrong");
            Inflater inf = new Inflater(true);
            inf.setInput(d, 0x28, csize);
            o.body = new byte[(int) size];
            try {
                int got = 0;
                while (got < size && !inf.finished()) {
                    int k = inf.inflate(o.body, got, (int) size - got);
                    if (k == 0 && (inf.needsInput() || inf.needsDictionary())) break;
                    got += k;
                }
                if (got != size) throw new Problem(name + " is cut short");
            } catch (DataFormatException e) {
                throw new Problem(name + " is damaged (" + e.getMessage() + ")");
            } finally {
                inf.end();
            }
        } else if (o.flags == FLAG_CITRUS) {
            if (citrus == null) throw new Problem(name + " is encrypted");
            Citrus.Opened c = null;
            for (long id : steamIds) {
                c = citrus.decrypt(d, id, curveHint);
                o.steamId = id;
                if (c != null) break;
            }
            if (c == null) throw new Citrus.NeedId(steamIds.length == 0);
            o.body = c.body;
            o.curve = c.curve;
            o.steam = true;
        } else {
            throw new Problem(name + ": a kind of save this doesn't know (flags " + o.flags + ")");
        }
        // Plain and deflated files read at 12 (the Switch's layout); Steam's at 16 or 24. A plain file from a save editor
        // could be either, so whichever reads.
        int[] bases = o.flags == FLAG_HAS_ID ? new int[] {STEAM_SS1_BASE} : o.flags == FLAG_CITRUS ? new int[] {STEAM_BASE}
                : new int[] {SWITCH_BASE, STEAM_BASE};
        Problem last = null;
        for (int base : bases) {
            try {
                o.roots = parse(o.body, base);
                o.base = base;
                // A plain file a save editor made from a Steam one can read at 12 too; what only Steam has gives it away
                if (base != SWITCH_BASE || containsRoot(o.roots, STEAM_ONLY_SLOT[1])) o.steam = true;
                // The Switch deflates its main files: a plain one is from another release (the Netflix one, say) or a
                // save editor, and gets filled into the Switch's layout like Steam's
                o.foreign = !o.steam && o.flags == 0 && (slot(name) || name.equals(SYSTEM));
                return o;
            } catch (Problem p) {
                last = p;
            }
        }
        throw new Problem(name + ": " + last.getMessage());
    }

    // The Switch's file: deflated, except the SS1_* ones
    static byte[] switchFile(String name, byte[] body) {
        if (name.startsWith("SS1_")) return plainFile(body);
        Deflater def = new Deflater(6, true);
        def.setInput(body);
        def.finish();
        ByteArrayOutputStream packed = new ByteArrayOutputStream(body.length / 4 + 64);
        byte[] buf = new byte[1 << 16];
        while (!def.finished()) packed.write(buf, 0, def.deflate(buf));
        def.end();
        byte[] stream = packed.toByteArray();
        ByteBuffer h = le(0x28);
        h.put(new byte[] {'D', 'S', 'S', 'S'}).putInt(2).putInt(FLAG_DEFLATE);
        h.put(body, 0, 4);  // (the game repeats the data's first word here)
        h.putLong(stream.length + 0x10L).putInt(1).putInt(stream.length).putLong(body.length);
        return withHash(h.array(), stream);
    }

    static byte[] plainFile(byte[] body) {
        ByteBuffer h = le(12);
        h.put(new byte[] {'D', 'S', 'S', 'S'}).putInt(2).putInt(0);
        return withHash(h.array(), body);
    }

    // Steam's SS1_* file: the account's number (the low half of the Steam ID) as a u64 at 0x10, then the data, laid out
    // at 24 already. (The 4 bytes before the number are only padding: whatever was in memory, in the game's files.)
    static byte[] steamSs1File(byte[] body, long steamId) {
        ByteBuffer h = le(0x18);
        h.put(new byte[] {'D', 'S', 'S', 'S'}).putInt(2).putInt(FLAG_HAS_ID).putInt(0).putLong(steamId & 0xffffffffL);
        return withHash(h.array(), body);
    }

    static final int STEAM_SS1_BASE = 24;

    // Steam's file, encrypted for that Steam ID; laid out at 16 already
    static byte[] steamFile(byte[] body, long steamId, Citrus citrus, int curve) throws Problem {
        ByteBuffer h = le(16);
        h.put(new byte[] {'D', 'S', 'S', 'S'}).putInt(2).putInt(FLAG_CITRUS).putInt(0);
        byte[] enc = citrus.encrypt(body, steamId, curve);
        byte[] out = withHash(h.array(), enc);
        Citrus.Opened back = citrus.decrypt(out, steamId, curve);  // (one more pass, but a bad save on the PC is worse)
        if (back == null || !Arrays.equals(back.body, body)) throw new Problem("the encrypted file doesn't read back");
        return out;
    }

    // A Steam ID as people write it: the 17-digit one (7656...), the account number Steam shows ([U:1:n] or just n).
    // 0 if it isn't one.
    static long steamId(String s) {
        if (s == null) return 0;
        s = s.trim();
        if (s.startsWith("[U:1:") && s.endsWith("]")) s = s.substring(5, s.length() - 1);
        try {
            long v = Long.parseLong(s);
            if (v > 0 && v < 1L << 32) return 0x0110000100000000L | v;
            return (v >>> 32) == 0x01100001L ? v : 0;
        } catch (NumberFormatException e) {
            return 0;
        }
    }

    // ---- Citrus ---------------------------------------------------------------------------------------------------------

    // Steam's encryption. The data goes in blocks of 0x40240 bytes: a key and IV (16 each), 0x200 bytes of the block's
    // real key and IV encrypted with EC ElGamal (four 64-bit halves; each one's point has x = value * 100 + something,
    // on one of 128 curves, with ~steamid64 as the private key) and then AES with the first key and IV, 0x40000 of data
    // in AES-128-CBC with the real ones, and a SHA3-256 of the key, IV, the plain 0x200 bytes and the plain data less its
    // last 32 bytes. Both AES passes also XOR every 16 bytes after the first with key ^ IV. After the blocks, 0x1000
    // bytes the game doesn't check and the data's size (u64). Which curve a Steam ID uses isn't known: it's found by
    // trying them all on a file it encrypted, and remembered (mhr.ini) to encrypt with later.
    static final class Citrus {
        static final class NeedId extends Problem {
            final boolean none;

            NeedId(boolean none) {
                super(none ? "it's encrypted with a Steam ID" : "that Steam ID doesn't open it");
                this.none = none;
            }
        }

        static final class Opened {
            byte[] body;
            int curve;
        }

        static final int KEYS = 0x200, DATA = 0x40000, BLOCK = 0x20 + KEYS + DATA + 0x20, TRAILER = 0x1000;

        private final BigInteger[][] curves = new BigInteger[128][];  // p, a, b, gx, gy
        private final SecureRandom random = new SecureRandom();

        Citrus(byte[] table) throws Problem {
            if (table.length != 128 * 5 * 32) throw new Problem("the curve table is damaged");
            for (int i = 0; i < 128; i++) {
                curves[i] = new BigInteger[5];
                for (int j = 0; j < 5; j++) curves[i][j] = leInt(table, (i * 5 + j) * 32, 32);
            }
        }

        private static BigInteger leInt(byte[] b, int at, int n) {
            byte[] be = new byte[n + 1];
            for (int i = 0; i < n; i++) be[n - i] = b[at + i];
            return new BigInteger(be);
        }

        private static void putLe(byte[] out, int at, BigInteger v) {
            byte[] be = v.toByteArray();
            for (int i = 0; i < 32; i++) out[at + i] = i < be.length ? be[be.length - 1 - i] : 0;
        }

        // Points: {x, y}, null for the one at infinity
        private static BigInteger[] add(BigInteger[] P, BigInteger[] Q, BigInteger a, BigInteger p) {
            if (P == null) return Q;
            if (Q == null) return P;
            BigInteger l;
            if (P[0].equals(Q[0])) {
                if (!P[1].equals(Q[1]) || P[1].signum() == 0) return null;
                l = P[0].pow(2).multiply(BigInteger.valueOf(3)).add(a).multiply(P[1].shiftLeft(1).modInverse(p)).mod(p);
            } else {
                l = Q[1].subtract(P[1]).multiply(Q[0].subtract(P[0]).mod(p).modInverse(p)).mod(p);
            }
            BigInteger x = l.multiply(l).subtract(P[0]).subtract(Q[0]).mod(p);
            return new BigInteger[] {x, l.multiply(P[0].subtract(x)).subtract(P[1]).mod(p)};
        }

        private static BigInteger[] mul(BigInteger k, BigInteger[] P, BigInteger a, BigInteger p) {
            BigInteger[] r = null;
            for (int i = k.bitLength() - 1; i >= 0; i--) {
                r = add(r, r, a, p);
                if (k.testBit(i)) r = add(r, P, a, p);
            }
            return r;
        }

        private static BigInteger privateKey(long steamId) {
            return new BigInteger(Long.toUnsignedString(~steamId));
        }

        // One 64-bit half from its 128 bytes; null if it isn't a point on this curve's math
        private static Long half(byte[] k, int at, BigInteger d, BigInteger[] c) {
            BigInteger p = c[0], a = c[1];
            BigInteger[] c1 = {leInt(k, at, 32), leInt(k, at + 32, 32)};
            BigInteger[] c2 = {leInt(k, at + 64, 32), leInt(k, at + 96, 32)};
            if (c1[0].compareTo(p) >= 0 || c2[0].compareTo(p) >= 0) return null;
            BigInteger[] s = mul(d, c1, a, p);
            if (s == null) return null;
            BigInteger[] m = add(c2, new BigInteger[] {s[0], s[1].negate().mod(p)}, a, p);
            if (m == null) return null;
            return m[0].divide(BigInteger.valueOf(100)).longValue();
        }

        private static byte[] realKeys(byte[] keys, BigInteger d, BigInteger[] c) {
            ByteBuffer out = le(32);
            for (int i = 0; i < 4; i++) {
                Long v = half(keys, i * 128, d, c);
                if (v == null) return null;
                out.putLong(v);
            }
            return out.array();
        }

        private static byte[] aes(boolean encrypt, byte[] buf, int at, int len, byte[] key, int keyAt) throws Problem {
            byte[] k = Arrays.copyOfRange(key, keyAt, keyAt + 16), iv = Arrays.copyOfRange(key, keyAt + 16, keyAt + 32);
            byte[] data = Arrays.copyOfRange(buf, at, at + len);
            try {
                Cipher c = Cipher.getInstance("AES/CBC/NoPadding");
                c.init(encrypt ? Cipher.ENCRYPT_MODE : Cipher.DECRYPT_MODE, new SecretKeySpec(k, "AES"), new IvParameterSpec(iv));
                if (encrypt) {
                    xorAfterFirst(data, k, iv);
                    return c.doFinal(data);
                }
                byte[] out = c.doFinal(data);
                xorAfterFirst(out, k, iv);
                return out;
            } catch (GeneralSecurityException e) {
                throw new Problem("AES: " + e.getMessage());
            }
        }

        private static void xorAfterFirst(byte[] d, byte[] k, byte[] iv) {
            for (int i = 16; i < d.length; i++) d[i] ^= (byte) (k[i & 15] ^ iv[i & 15]);
        }

        // null: no curve opens it with this ID (the wrong ID, most likely)
        Opened decrypt(byte[] file, long steamId, int curveHint) throws Problem {
            int end = file.length - 4;
            if (end < 0x10 + BLOCK + TRAILER + 8) throw new Problem("the encrypted file is too short");
            long size = le64(file, end - 8);
            int blocks = (end - 0x10 - TRAILER - 8) / BLOCK;
            if (size < 0 || size > (long) blocks * DATA) throw new Problem("the encrypted file's size is wrong");
            BigInteger d = privateKey(steamId);
            byte[] keys = aes(false, file, 0x10 + 0x20, KEYS, file, 0x10);
            int curve = -1;
            byte[] first = null;
            for (int n = -1; n < 128 && curve < 0; n++) {
                int i = n < 0 ? curveHint : n;
                if (i < 0 || i >= 128 || (n >= 0 && i == curveHint)) continue;
                byte[] real = realKeys(keys, d, curves[i]);
                if (real == null) continue;
                // The right key gives the game's data, which is mostly zeros; a wrong one gives noise
                byte[] t = aes(false, file, 0x10 + 0x20 + KEYS, DATA, real, 0);
                int zeros = 0;
                for (byte b : t) zeros += b == 0 ? 1 : 0;
                if (zeros > DATA / 2) {
                    curve = i;
                    first = t;
                }
            }
            if (curve < 0) return null;
            byte[] body = new byte[(int) size];
            for (int i = 0, got = 0; i < blocks && got < size; i++) {
                int at = 0x10 + i * BLOCK;
                byte[] t = first;
                if (i > 0) {
                    byte[] real = realKeys(aes(false, file, at + 0x20, KEYS, file, at), d, curves[curve]);
                    if (real == null) throw new Problem("block " + i + " doesn't open");
                    t = aes(false, file, at + 0x20 + KEYS, DATA, real, 0);
                }
                int n = (int) Math.min(DATA, size - got);
                System.arraycopy(t, 0, body, got, n);
                got += n;
            }
            Opened o = new Opened();
            o.body = body;
            o.curve = curve;
            return o;
        }

        byte[] encrypt(byte[] body, long steamId, int curve) throws Problem {
            BigInteger[] c = curves[curve];
            BigInteger p = c[0], a = c[1], b = c[2];
            BigInteger[] g = {c[3], c[4]};
            BigInteger[] pub = mul(privateKey(steamId), g, a, p);
            int blocks = Math.max(1, (body.length + DATA - 1) / DATA);
            byte[] out = new byte[blocks * BLOCK + TRAILER + 8];
            for (int i = 0; i < blocks; i++) {
                int at = i * BLOCK;
                byte[] outer = new byte[32], real = new byte[32];
                random.nextBytes(outer);
                random.nextBytes(real);
                byte[] keys = new byte[KEYS];
                ByteBuffer halves = ByteBuffer.wrap(real).order(ByteOrder.LITTLE_ENDIAN);
                for (int h = 0; h < 4; h++) {
                    BigInteger[] m = point(BigInteger.valueOf(halves.getLong(h * 8)).and(MASK64).multiply(BigInteger.valueOf(100)), a, b, p);
                    BigInteger k;
                    do k = new BigInteger(64, random); while (k.signum() == 0);
                    BigInteger[] c1 = mul(k, g, a, p), c2 = add(m, mul(k, pub, a, p), a, p);
                    if (c1 == null || c2 == null) throw new Problem("a key didn't encrypt");
                    putLe(keys, h * 128, c1[0]);
                    putLe(keys, h * 128 + 32, c1[1]);
                    putLe(keys, h * 128 + 64, c2[0]);
                    putLe(keys, h * 128 + 96, c2[1]);
                }
                byte[] plain = new byte[DATA];
                System.arraycopy(body, i * DATA, plain, 0, Math.min(DATA, body.length - i * DATA));
                System.arraycopy(outer, 0, out, at, 32);
                System.arraycopy(aes(true, keys, 0, KEYS, outer, 0), 0, out, at + 0x20, KEYS);
                System.arraycopy(aes(true, plain, 0, DATA, real, 0), 0, out, at + 0x20 + KEYS, DATA);
                Sha3 sha = new Sha3();
                sha.update(outer, 0, 32);
                sha.update(keys, 0, KEYS);
                sha.update(plain, 0, DATA - 0x20);
                System.arraycopy(sha.digest(), 0, out, at + 0x20 + KEYS + DATA, 32);
            }
            ByteBuffer.wrap(out).order(ByteOrder.LITTLE_ENDIAN).putLong(blocks * BLOCK + TRAILER, body.length);
            return out;
        }

        private static final BigInteger MASK64 = BigInteger.ONE.shiftLeft(64).subtract(BigInteger.ONE);

        // A point whose x is value * 100 + j for the first j that's on the curve (decrypting divides by 100)
        private static BigInteger[] point(BigInteger x100, BigInteger a, BigInteger b, BigInteger p) throws Problem {
            for (int j = 0; j < 100; j++) {
                BigInteger x = x100.add(BigInteger.valueOf(j));
                BigInteger rhs = x.pow(3).add(a.multiply(x)).add(b).mod(p);
                BigInteger y = sqrt(rhs, p);
                if (y != null) return new BigInteger[] {x, y};
            }
            throw new Problem("no point for a key");
        }

        // Square root mod a prime (Tonelli-Shanks; the quick way when p % 4 == 3); null if there's none
        private static BigInteger sqrt(BigInteger n, BigInteger p) {
            if (n.signum() == 0) return n;
            BigInteger one = BigInteger.ONE, pm1 = p.subtract(one);
            if (!n.modPow(pm1.shiftRight(1), p).equals(one)) return null;
            if (p.testBit(1)) return n.modPow(p.add(one).shiftRight(2), p);
            BigInteger q = pm1;
            int s = 0;
            while (!q.testBit(0)) {
                q = q.shiftRight(1);
                s++;
            }
            BigInteger z = BigInteger.valueOf(2);
            while (z.modPow(pm1.shiftRight(1), p).equals(one)) z = z.add(one);
            BigInteger c = z.modPow(q, p), r = n.modPow(q.add(one).shiftRight(1), p), t = n.modPow(q, p);
            int m = s;
            while (!t.equals(one)) {
                int i = 0;
                for (BigInteger t2 = t; !t2.equals(one); t2 = t2.multiply(t2).mod(p)) i++;
                BigInteger bb = c.modPow(one.shiftLeft(m - i - 1), p);
                r = r.multiply(bb).mod(p);
                c = bb.multiply(bb).mod(p);
                t = t.multiply(c).mod(p);
                m = i;
            }
            return r;
        }
    }

    // SHA3-256 (FIPS 202), since Android only has it from API 34
    static final class Sha3 {
        private static final long[] RC = {
            0x0000000000000001L, 0x0000000000008082L, 0x800000000000808aL, 0x8000000080008000L, 0x000000000000808bL,
            0x0000000080000001L, 0x8000000080008081L, 0x8000000000008009L, 0x000000000000008aL, 0x0000000000000088L,
            0x0000000080008009L, 0x000000008000000aL, 0x000000008000808bL, 0x800000000000008bL, 0x8000000000008089L,
            0x8000000000008003L, 0x8000000000008002L, 0x8000000000000080L, 0x000000000000800aL, 0x800000008000000aL,
            0x8000000080008081L, 0x8000000000008080L, 0x0000000080000001L, 0x8000000080008008L};
        private static final int[] ROT = {0, 1, 62, 28, 27, 36, 44, 6, 55, 20, 3, 10, 43, 25, 39, 41, 45, 15, 21, 8, 18, 2, 61, 56, 14};
        private static final int RATE = 136;
        private final long[] s = new long[25];
        private final byte[] buf = new byte[RATE];
        private int n;

        void update(byte[] b, int at, int len) {
            for (int i = 0; i < len; i++) {
                buf[n++] = b[at + i];
                if (n == RATE) absorb();
            }
        }

        private void absorb() {
            for (int i = 0; i < RATE / 8; i++) s[i] ^= le64(buf, i * 8);
            permute();
            n = 0;
        }

        byte[] digest() {
            Arrays.fill(buf, n, RATE, (byte) 0);
            buf[n] ^= 0x06;
            buf[RATE - 1] ^= (byte) 0x80;
            absorb();
            ByteBuffer out = le(32);
            for (int i = 0; i < 4; i++) out.putLong(s[i]);
            return out.array();
        }

        private void permute() {
            long[] c = new long[5], b = new long[25];
            for (long rc : RC) {
                for (int x = 0; x < 5; x++) c[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
                for (int x = 0; x < 5; x++) {
                    long d = c[(x + 4) % 5] ^ Long.rotateLeft(c[(x + 1) % 5], 1);
                    for (int y = 0; y < 25; y += 5) s[y + x] ^= d;
                }
                for (int x = 0; x < 5; x++)
                    for (int y = 0; y < 5; y++) b[y + 5 * ((2 * x + 3 * y) % 5)] = Long.rotateLeft(s[x + 5 * y], ROT[x + 5 * y]);
                for (int y = 0; y < 25; y += 5)
                    for (int x = 0; x < 5; x++) s[y + x] = b[y + x] ^ (~b[y + (x + 1) % 5] & b[y + (x + 2) % 5]);
                s[0] ^= rc;
            }
        }
    }
}
