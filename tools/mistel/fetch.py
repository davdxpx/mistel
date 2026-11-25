"""`mistel update-check`: fetches the newest version of a file from its `[update]` address, as a runtime does
(spec, section 17.3), so that an author can test the address before players use it.

The request is the one every runtime makes: GET, the address as it is written, the User-Agent `mncf-update/1`, no
cookies, no credentials, no cache validators and no Referer; at most 5 redirects, each to an https address; and no
connection to a loopback, private or link-local address.
"""
import http.client
import ipaddress
import socket
import ssl
import urllib.parse
from dataclasses import dataclass, field

from mncf import MAX_FILE, update
from mncf.pack import MAX_TOTAL


class FetchError(ValueError):
    pass


@dataclass
class Fetched:
    data: bytes
    url: str                                        # the address the answer came from, after redirects
    hops: list = field(default_factory=list)        # every address asked for, in order


def _public(host, port):
    """Refuses a host whose addresses are loopback, private, link-local or otherwise not public."""
    try:
        infos = socket.getaddrinfo(host, port, type=socket.SOCK_STREAM)
    except socket.gaierror as e:
        raise FetchError(f'{host}: the name does not resolve ({e.strerror or e})') from None
    for info in infos:
        addr = ipaddress.ip_address(info[4][0].split('%')[0])
        if addr.is_loopback or addr.is_private or addr.is_link_local or addr.is_multicast or addr.is_reserved or \
                addr.is_unspecified:
            raise FetchError(f'{host} resolves to {addr}, an address that is not public')


def _get(url, limit, timeout, connect):
    parts = urllib.parse.urlsplit(url)
    if parts.scheme != 'https' or not parts.hostname:
        raise FetchError(f'{url}: not an https address')
    host, port = parts.hostname, parts.port or 443
    connect(host, port)
    conn = http.client.HTTPSConnection(host, port, timeout=timeout, context=ssl.create_default_context())
    path = parts.path or '/'
    if parts.query:
        path += '?' + parts.query
    try:
        conn.putrequest('GET', path, skip_accept_encoding=True)
        conn.putheader('User-Agent', update.USER_AGENT)
        conn.putheader('Accept-Encoding', 'identity')
        conn.endheaders()
        resp = conn.getresponse()
        if resp.status in (301, 302, 303, 307, 308):
            return resp.status, resp.getheader('Location'), b''
        if resp.status != 200:
            return resp.status, None, b''
        data = resp.read(limit + 1)
        if len(data) > limit:
            raise FetchError(f'{url}: the answer is larger than {limit} bytes')
        return 200, None, data
    except (OSError, http.client.HTTPException) as e:
        raise FetchError(f'{url}: {e}') from None
    finally:
        conn.close()


def fetch(url, pack=False, timeout=30, get=None, connect=None):
    """The bytes at an `[update]` address, following redirects as a runtime does. Raises FetchError. `get` and
    `connect` stand in for the request and the address check, for tests."""
    get, connect = get or _get, connect or _public
    if update.url_host(url) is None:
        raise FetchError(f'{url}: not an address section 17.3 allows')
    limit = MAX_TOTAL if pack else MAX_FILE
    hops, current = [], url
    for _ in range(update.MAX_REDIRECTS + 1):
        hops.append(current)
        status, location, data = get(current, limit, timeout, connect)
        if status == 200:
            return Fetched(data, current, hops)
        if location is None:
            raise FetchError(f'{current}: the answer has status {status}, not 200')
        nxt = urllib.parse.urljoin(current, location)
        if urllib.parse.urlsplit(nxt).scheme != 'https':
            raise FetchError(f'{current} redirects to {nxt}, which is not https')
        current = nxt
    raise FetchError(f'{url}: more than {update.MAX_REDIRECTS} redirects')
