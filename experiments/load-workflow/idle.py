"""Quiet-window check shared by the performance experiments."""
import math


def wait_for_quiet(sample, maximum):
    if not math.isfinite(maximum) or not 0<=maximum<=100:
        raise ValueError('CPU limit must be between 0 and 100 percent')
    quiet=[]
    while len(quiet)<2:
        value=sample()
        if not math.isfinite(value) or not 0<=value<=100:
            raise ValueError('Invalid CPU measurement')
        quiet=quiet+[value] if value<=maximum else []
    return quiet
