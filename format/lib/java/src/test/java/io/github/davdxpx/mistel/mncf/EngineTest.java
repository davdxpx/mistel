package io.github.davdxpx.mistel.mncf;

import io.github.davdxpx.mistel.mncf.testing.Check;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.List;

/** Tests of the engine's API: the parts the pass cases of the conformance suite do not reach. */
public final class EngineTest {

    private EngineTest() {
    }

    private static final String FILE = "format = \"mncf/1\"\n"
            + "name = \"Example\"\n"
            + "\n"
            + "[game]\n"
            + "platform = \"switch\"\n"
            + "id = \"0100000000ABC000\"\n"
            + "\n"
            + "[[cheat]]\n"
            + "id = \"money\"\n"
            + "name = \"Money\"\n"
            + "default = true\n"
            + "toggle = \"L+R\"\n"
            + "  [cheat.values.count]\n"
            + "  type = \"u32\"\n"
            + "  default = 5\n"
            + "  [[cheat.write]]\n"
            + "  at = { region = \"heap\", offset = 0x10 }\n"
            + "  type = \"u32\"\n"
            + "  value = \"$count\"\n";

    private static final byte[] HEAP = new byte[0x100];

    private static Engine engine() throws MncfException {
        MncfFile f = Mncf.load(FILE.getBytes(StandardCharsets.UTF_8));
        Runtime rt = Runtime.builder("switch").region("heap", 0x10000000L, HEAP.length).memory(new GuestMemory() {
            @Override
            public byte[] read(long address, int size) {
                long off = address - 0x10000000L;
                return off >= 0 && off + size <= HEAP.length ? Arrays.copyOfRange(HEAP, (int) off, (int) off + size) : null;
            }
        }).build();
        return new Engine(f, rt, null);
    }

    public static void testLoadIsTheEvaluation() throws Exception {
        Engine e = engine();
        Check.equal("on", e.evaluation().cheat("money").state());
        Check.equal("on", e.state("money").state());
        Check.equal(null, e.build());
    }

    public static void testTickWithTheRuntimesMemory() throws Exception {
        Engine e = engine();
        Pass p = e.tick(null, null);
        Check.equal(1L, p.number());
        Check.equal(1, p.changes().size());
        Change w = p.changes().get(0);
        Check.equal("write", w.kind());
        Check.equal("money", w.cheat());
        Check.equal(0x10000010L, w.address());
        Check.isTrue(Arrays.equals(new byte[] {5, 0, 0, 0}, w.bytes()), "the bytes of the value");
        Check.equal("on", p.cheat("money").state());
        Check.isTrue(p.cheat("nothing") == null, "no such cheat");
    }

    public static void testPressAsAList() throws Exception {
        Engine e = engine();
        e.press(Arrays.asList("R", "L"));
        Pass p = e.tick(null, null);
        Check.equal("off", p.cheat("money").state());
        Check.equal(0, p.changes().size());
    }

    public static void testValuesOfAnyWholeNumberType() throws Exception {
        Engine e = engine();
        e.setValue("money.count", 7);
        List<Change> changes = e.tick(null, null).changes();
        Check.isTrue(Arrays.equals(new byte[] {7, 0, 0, 0}, changes.get(0).bytes()), "an int is a whole number");
        e.setValue("money.count", -1L);
        Check.equal("value-out-of-range", e.state("money").reason());
    }

    public static void testMistakesOfTheCaller() throws Exception {
        final Engine e = engine();
        Check.throwsWith(IllegalArgumentException.class, "no cheat", new Check.Action() {
            @Override
            public void run() {
                e.switchCheat("nothing", true);
            }
        });
        Check.throwsWith(IllegalArgumentException.class, "no value", new Check.Action() {
            @Override
            public void run() {
                e.setValue("money.other", 1);
            }
        });
        Check.throwsWith(IllegalArgumentException.class, "a value is", new Check.Action() {
            @Override
            public void run() {
                e.setValue("money.count", "many");
            }
        });
        Check.throwsWith(IllegalArgumentException.class, "rate", new Check.Action() {
            @Override
            public void run() throws Exception {
                new Engine(Mncf.load(FILE.getBytes(StandardCharsets.UTF_8)), Runtime.builder("switch").build(), null, 0);
            }
        });
    }

    public static void testStopChangesNothingWithoutCode() throws Exception {
        Engine e = engine();
        e.tick(null, null);
        Check.equal(0, e.stop().size());
        Check.equal("off", e.state("money").state());
        Check.equal(0, e.tick(null, null).changes().size());
    }
}
