package io.github.davdxpx.mistel.mncf.testing;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.List;

/**
 * Runs tests without a test framework. Each argument names a test class, and each public static
 * void method of it without parameters whose name starts with "test" is a test. Prints one line
 * per failure and a summary, and exits with 1 when a test failed.
 */
public final class RunTests {

    private RunTests() {
    }

    /** Runs the tests of the classes named in args. */
    public static void main(String[] args) {
        if (args.length == 0) {
            System.err.println("usage: RunTests <test class>...");
            System.exit(2);
        }
        int passed = 0;
        int failed = 0;
        for (String className : args) {
            List<Method> tests;
            try {
                tests = testsOf(Class.forName(className));
            } catch (ClassNotFoundException | LinkageError e) {
                System.out.println("FAIL " + className + ": cannot load the class: " + e);
                failed++;
                continue;
            }
            if (tests.isEmpty()) {
                System.out.println("FAIL " + className + ": no tests");
                failed++;
            }
            for (Method test : tests) {
                Throwable problem = run(test);
                if (problem == null) {
                    passed++;
                } else {
                    failed++;
                    System.out.println("FAIL " + test.getDeclaringClass().getSimpleName() + "." + test.getName()
                            + where(problem, test) + ": " + describe(problem));
                }
            }
        }
        System.out.println(passed + " passed, " + failed + " failed");
        System.exit(failed == 0 ? 0 : 1);
    }

    private static List<Method> testsOf(Class<?> type) {
        List<Method> tests = new ArrayList<>();
        for (Method method : type.getDeclaredMethods()) {
            int modifiers = method.getModifiers();
            if (method.getName().startsWith("test") && Modifier.isPublic(modifiers)
                    && Modifier.isStatic(modifiers) && method.getReturnType() == void.class
                    && method.getParameterTypes().length == 0) {
                tests.add(method);
            }
        }
        tests.sort(Comparator.comparing(Method::getName));
        return tests;
    }

    private static Throwable run(Method test) {
        try {
            test.invoke(null);
            return null;
        } catch (InvocationTargetException e) {
            return e.getCause();
        } catch (IllegalAccessException e) {
            return e;
        }
    }

    /** Returns the line of the test method where the problem came up, such as " (FooTest.java:12)". */
    private static String where(Throwable problem, Method test) {
        for (StackTraceElement frame : problem.getStackTrace()) {
            if (frame.getClassName().equals(test.getDeclaringClass().getName())
                    && frame.getMethodName().equals(test.getName())) {
                return " (" + frame.getFileName() + ":" + frame.getLineNumber() + ")";
            }
        }
        return "";
    }

    private static String describe(Throwable problem) {
        return problem instanceof AssertionError ? String.valueOf(problem.getMessage()) : problem.toString();
    }
}
