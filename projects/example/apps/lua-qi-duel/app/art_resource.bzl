"""Embed a lossless art resource using the repository's byte-array generator."""
load("@rules_cc//cc:defs.bzl", "cc_library")

def qi_duel_art(name, src, symbol):
    native.genrule(
        name = name + "_generated",
        srcs = [src],
        outs = [name + "_generated.h", name + "_generated.c"],
        tools = ["//libs/lua:embed_resource"],
        cmd = "$(location //libs/lua:embed_resource) --source $(SRCS) " +
              "--header $(location " + name + "_generated.h) " +
              "--implementation $(location " + name + "_generated.c) --symbol " + symbol,
    )
    cc_library(
        name = name,
        srcs = [":" + name + "_generated.c"],
        hdrs = [":" + name + "_generated.h"],
        strip_include_prefix = ".",
    )
