// Builtins the docs promise but nothing tested: environment access, stderr
// output, string padding, the clock and the collector. The regression suite
// runs this through BOTH backends, so any drift between them fails here.

// env_or falls back when the variable is unset; env reads what setenv wrote.
print(env_or("VAYU_TEST_UNSET_NAME", "fallback"))
setenv("VAYU_TEST_SET_NAME", "written")
print(env("VAYU_TEST_SET_NAME"))
print(env_or("VAYU_TEST_SET_NAME", "fallback"))

// pad pads on the left by default, with an optional fill character, and never
// truncates. The function form takes the string as its first argument.
print("ab".pad(5) + "|")
print("ab".pad(5, 46) + "|")
print(pad("xy", 4) + "|")
print(pad("xy", 4, 46) + "|")
print("toolong".pad(3))
print(pad("xy", 4, 46, "right") + "|")

// eprint renders like print but writes to stderr, keeping stdout clean.
eprint("this line goes to stderr")

// time.clock is monotonic seconds as a Float; time.now is epoch seconds as Int.
print(type(time.clock()), type(time.now()))
a = time.clock()
b = time.clock()
print(b >= a)

// gc() collects, gc("stats") reports, gc("reset") clears the counters.
print(type(gc("stats")))
print(gc("stats") != "")
gc("reset")
print("gc ok")
