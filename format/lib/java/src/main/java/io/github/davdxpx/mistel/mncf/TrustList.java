package io.github.davdxpx.mistel.mncf;

import java.util.ArrayList;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;

/**
 * The keys a reader trusts, by name, and the key ids it no longer trusts (section 12). A caller
 * fills it in before reading files; reading does not change it.
 */
public final class TrustList {

    private final Map<String, List<Key>> keys = new LinkedHashMap<>();
    private final Set<String> revoked = new LinkedHashSet<>();

    /** A trusted public key and the name it is shown with. */
    static final class Key {
        final String name;
        final byte[] publicKey;

        Key(String name, byte[] publicKey) {
            this.name = name;
            this.publicKey = publicKey;
        }
    }

    /**
     * Trusts a public key under a name. A key may be trusted under several names.
     *
     * @param publicKey the 32 bytes of an Ed25519 public key
     * @return this list
     * @throws IllegalArgumentException if the key is not 32 bytes
     */
    public TrustList trust(String name, byte[] publicKey) {
        if (publicKey.length != 32) {
            throw new IllegalArgumentException("an Ed25519 public key is 32 bytes");
        }
        String kid = Signatures.keyId(publicKey);
        List<Key> list = keys.get(kid);
        if (list == null) {
            list = new ArrayList<>();
            keys.put(kid, list);
        }
        list.add(new Key(name, publicKey.clone()));
        return this;
    }

    /**
     * Stops trusting a key id: a signature by it makes a file "revoked".
     *
     * @param keyId the 16 hexadecimal digits of the key id, in any case
     * @return this list
     */
    public TrustList revoke(String keyId) {
        revoked.add(Py.lower(keyId));
        return this;
    }

    /** Returns the key ids this list no longer trusts, in lower case. */
    public Set<String> revoked() {
        return Collections.unmodifiableSet(new LinkedHashSet<>(revoked));
    }

    boolean isRevoked(String keyId) {
        return revoked.contains(keyId);
    }

    List<Key> keys(String keyId) {
        List<Key> list = keys.get(keyId);
        return list == null ? Collections.<Key>emptyList() : list;
    }
}
