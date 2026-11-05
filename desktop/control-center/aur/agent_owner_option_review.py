"""Exact-config-bound option-name review; never execute or accept an option."""
import hashlib
import json
import agent_owner_config_review as base
EXPECTED='250266c8436ebf3f94595d564f042fc5691b7d7104ca4f71c4d4f4ac1f804bae'
# Names are static output labels from the official GnuPG Agent Options manual.
# Matching a name is NOT version support, argument validation or policy approval.
GROUPS={
 'pinentry-execution':('pinentry-program','pinentry-touch-file'),
 'cache-policy':('allow-preset-passphrase','ignore-cache-for-signing','default-cache-ttl','max-cache-ttl','default-cache-ttl-ssh','max-cache-ttl-ssh','no-allow-external-cache'),
 'pinentry-policy':('allow-loopback-pinentry','no-allow-loopback-pinentry','allow-emacs-pinentry','pinentry-timeout','batch'),
 'logging-debug':('log-file','debug','debug-level','debug-all','debug-pinentry','verbose','quiet'),
 'socket-control':('extra-socket','browser-socket','enable-ssh-support','disable-check-own-socket','steal-socket','use-standard-socket','no-use-standard-socket','use-standard-socket-p'),
 'lifecycle-or-config':('homedir','options','daemon','no-detach','supervised','server'),
 'smartcard':('scdaemon-program','disable-scdaemon'),
 'trust-policy':('no-allow-mark-trusted','no-user-trustlist','sys-trustlist-name'),
}
NAMES={name:group for group,names in GROUPS.items() for name in names}


def describe(raw,expected=EXPECTED):
    if hashlib.sha256(raw).hexdigest()!=expected:raise ValueError('Exact configuration mismatch')
    if len(raw)>base.LIMIT:raise ValueError('Bound exceeded')
    try:text=raw.decode('ascii')
    except UnicodeDecodeError:return {'classification':'REQUIRES_REVIEW','reason':'non-ascii','options':[]}
    rows=[]
    for line in text.splitlines():
        line=line.strip()
        if not line or line.startswith('#'):continue
        # No input token is ever emitted. Only finite dictionary keys can appear.
        token=line.split()[0]
        name=token.split('=',1)[0]
        known=name in NAMES
        rows.append({'option':name if known else 'UNRECOGNIZED_REDACTED',
                     'effect':NAMES[name] if known else 'unknown',
                     'values':'REDACTED','policy_acceptance':'NOT_APPROVED',
                     'syntax':'needs-review' if '=' in token or token.startswith('-') else 'name-only-reviewed'})
    return {'classification':'REQUIRES_REVIEW','options':rows,'active_line_count':len(rows),
            'version_support':'NOT_VERIFIED','exact_config_binding':'PASS'}


def main():
    original=base.classify
    try:
        base.classify=describe
        return base.main()
    finally:base.classify=original

if __name__=='__main__':raise SystemExit(main())
