# SMS send crash from a non-virtual PDU destructor

## Symptom

TestFlight build 260763 (commit `8c4e881fa`) crashed with `EXC_BREAKPOINT`
while an application was sending an SMS. The top frame was
`schedule_copy_operation::execute`, at offset `+2964`; the faulting instruction
was `brk #1` rather than a bad memory access.

The build's dSYM UUID (`97E4AF51-FE80-319F-BB41-48AC2BF3ACE6`) exactly matches
the crash image. Although the optimized dSYM identifies only the containing
function, the SMS implementation was unchanged in a local Release build. Its
instruction at the same function-relative offset is also `brk #1`. The crash
LR matches the instruction immediately after freeing the PDU during the local
`sms_header` destructor sequence.

## Root cause

`sms_message::pdu_` is a `std::unique_ptr<sms_pdu>`. Deserialization creates an
`sms_submit` and stores it through that base pointer, but `sms_pdu` was an
abstract polymorphic class without a virtual destructor:

```cpp
struct sms_pdu {
    virtual void absorb(common::chunkyseri &seri) = 0;
};
```

When `schedule_copy_operation::execute` finished editing a successfully parsed
message store, the local `sms_header` was destroyed. Its non-null PDU was then
deleted through `sms_pdu*`. That is undefined behavior: the actual
`sms_submit` object cannot be destroyed correctly through a base class with a
non-virtual destructor. In the optimized Apple Clang build, the impossible
abstract-base deletion path was lowered to the observed inline `brk #1`.

The recipient and command-buffer checks were red herrings. The trap occurs
only after a valid submit PDU has been parsed, precisely because that is what
makes the polymorphic pointer non-null.

## Fix

`sms_pdu` now has a virtual defaulted destructor, so `unique_ptr<sms_pdu>`
destroys the concrete `sms_submit` object normally. A compile-time regression
test requires the base to retain a virtual destructor; this catches the
lifetime contract directly without depending on optimizer-specific trap code.

The fixed Release binary confirms that the old conditional branch to `brk #1`
has become a virtual destructor call. The standard iOS regression passed twice
(first run with the new app installed, second without reinstalling), each at
11/11 across Final Battle, 5320 Calculator, and N95 Calculator. The final
emulator log contains no guest crash, access violation, graphics halt, or host
fatal signal.
