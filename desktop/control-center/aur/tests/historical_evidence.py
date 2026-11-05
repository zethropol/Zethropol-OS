"""Opt-in archived host captures; never substitute fixtures for real evidence.

The default portable suite runs all core AUR security cases. These historical
rollout checks additionally require the separately retained, exact pinned public
captures in build/ and (for some checks) the original public host policy.
Setting the flag with missing/changed evidence MUST fail the affected tests.
"""
import os
import unittest

requires_historical_evidence = unittest.skipUnless(
    os.environ.get('ZETHROPOL_TEST_HISTORICAL_EVIDENCE') == '1',
    'historical host capture test: requires separately retained pinned public evidence; opt in with ZETHROPOL_TEST_HISTORICAL_EVIDENCE=1',
)
