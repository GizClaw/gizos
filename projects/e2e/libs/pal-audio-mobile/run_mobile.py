"""Audio-specific frame, stability and allocator observations."""


def verify_report(report, args):
    limits = args.contract["options"]
    assert report["mic_frames"] >= limits["min_frames"] and report["speaker_frames"] >= limits["min_frames"]
    assert report["stability_elapsed_ms"] >= limits["stability_ms"]
    assert report["output_peak"] > 0
    assert report["allocator_allocations"] > 0
    assert report["allocator_allocations"] == report["allocator_frees"]
