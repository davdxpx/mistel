package io.github.davdxpx.mistel.mncf.testing;

import java.util.Objects;

/** Assertions for tests that {@link RunTests} runs. A failed check throws an {@link AssertionError}. */
public final class Check {

    /** Code that a test expects to throw. */
    public interface Action {
        /** Runs the code. */
        void run() throws Exception;
    }

    private Check() {
    }

    /** Fails unless expected and actual are equal, as {@link Objects#equals} decides. */
    public static void equal(Object expected, Object actual) {
        if (!Objects.equals(expected, actual)) {
            throw new AssertionError("expected " + show(expected) + ", got " + show(actual));
        }
    }

    /** Fails with the message unless the condition holds. */
    public static void isTrue(boolean condition, String message) {
        if (!condition) {
            throw new AssertionError(message);
        }
    }

    /** Fails with the message. */
    public static void fail(String message) {
        throw new AssertionError(message);
    }

    /**
     * Fails unless the action throws an exception of the given type whose message contains the
     * fragment. Returns the exception, for further checks.
     */
    public static <T extends Throwable> T throwsWith(Class<T> type, String fragment, Action action) {
        try {
            action.run();
        } catch (Throwable thrown) {
            if (!type.isInstance(thrown)) {
                throw new AssertionError("expected " + type.getSimpleName() + ", got " + thrown, thrown);
            }
            String message = String.valueOf(thrown.getMessage());
            if (!message.contains(fragment)) {
                throw new AssertionError("expected a message with \"" + fragment + "\", got \"" + message + "\"");
            }
            return type.cast(thrown);
        }
        throw new AssertionError("expected " + type.getSimpleName() + " with \"" + fragment
                + "\", but nothing was thrown");
    }

    private static String show(Object value) {
        if (value == null) {
            return "null";
        }
        if (value instanceof String) {
            return "\"" + value + "\"";
        }
        return value + " (" + value.getClass().getSimpleName() + ")";
    }
}
