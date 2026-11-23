#!/bin/sh
# Builds the Java reader and runs its tests, the conformance suite in ../../tests among them.
#
# Needs a JDK 9 or newer: javac and java from $JAVA_HOME/bin when JAVA_HOME is set, else from PATH.
# When ANDROID_JAR names an android.jar, the main sources compile against it, so that no Java API
# that Android lacks can slip in. Otherwise they compile against the Java 8 API.
# When D8 also names Android's d8 tool, the main classes are dexed with it for API 33, as a check
# that the library runs on Android 13.
set -eu

cd "$(dirname "$0")"
if [ -n "${JAVA_HOME:-}" ]; then
    javac="$JAVA_HOME/bin/javac"
    java="$JAVA_HOME/bin/java"
else
    javac=javac
    java=java
fi
case "$(uname -s)" in
    MINGW* | MSYS* | CYGWIN*) sep=';' ;;
    *) sep=':' ;;
esac

rm -rf build
mkdir -p build/classes build/test-classes
find src/main/java -name '*.java' > build/main-sources.txt
find src/test/java -name '*.java' > build/test-sources.txt

if [ -n "${ANDROID_JAR:-}" ]; then
    "$javac" -encoding UTF-8 -Xlint:all,-options -source 8 -target 8 -bootclasspath "$ANDROID_JAR" \
        -d build/classes @build/main-sources.txt
else
    "$javac" -encoding UTF-8 -Xlint:all,-options --release 8 -d build/classes @build/main-sources.txt
fi
"$javac" -encoding UTF-8 -Xlint:all,-options --release 8 -cp build/classes -d build/test-classes \
    @build/test-sources.txt

if [ -n "${D8:-}" ]; then
    if [ -z "${ANDROID_JAR:-}" ]; then
        echo "build.sh: D8 needs ANDROID_JAR as well" >&2
        exit 2
    fi
    mkdir -p build/dex
    find build/classes -name '*.class' > build/main-classes.txt
    "$D8" --release --min-api 33 --lib "$ANDROID_JAR" --output build/dex @build/main-classes.txt
    echo "d8: dexed $(wc -l < build/main-classes.txt | tr -d ' ') classes for API 33 into build/dex"
fi

tests=$(find src/test/java -name '*Test.java' | sed -e 's|^src/test/java/||' -e 's|\.java$||' -e 's|/|.|g' | sort)
# Test class names have no spaces, so $tests is split into words on purpose.
"$java" -Dmncf.tests=../../tests -cp "build/classes${sep}build/test-classes" \
    io.github.davdxpx.mistel.mncf.testing.RunTests $tests
