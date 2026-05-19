<!--
Copyright (c) 2026 Edward Boggis-Rolfe
All rights reserved.
-->

# SGX Enclave Identity Developer Guide

## Purpose

This guide is for application developers who need to decide which SGX enclaves
their service should trust, without becoming SGX attestation specialists.

The important idea is simple:

1. The enclave is built and signed.
2. The signed enclave has SGX identity fields such as `MRENCLAVE`,
   `MRSIGNER`, `ISVPRODID`, `ISVSVN`, and debug attributes.
3. Remote or local attestation proves that the peer is running an enclave with
   those identity fields.
4. Application policy compares the attested fields with the expected release
   identity.

In normal application policy, you do not match the raw signature bytes. You
match the identity fields that the signature and SGX quote/report establish.

## What "Signature" Means

SGX uses a signing step after the enclave shared object is linked. The Intel
`sgx_sign` tool creates a signed enclave image containing a `SIGSTRUCT`.
`SIGSTRUCT` includes the enclave measurement, product/security version, signer
information, and an RSA signature over the enclave signing structure.

The fields developers normally care about are:

| Field | Meaning | Typical policy use |
|---|---|---|
| `MRENCLAVE` | Measurement of the exact enclave build and layout | Pin an exact binary release |
| `MRSIGNER` | Hash identity of the enclave signing public key | Trust all approved releases from one signer |
| `ISVPRODID` | Product id chosen in enclave config | Separate products under the same signer |
| `ISVSVN` | Security version chosen in enclave config | Enforce minimum patched version |
| `DEBUG` attribute | Whether the enclave was built/debug-enabled | Reject debug enclaves in production |

`MRENCLAVE` is strict. Any meaningful change to enclave code, linked enclave
libraries, layout, or signing-relevant config can change it. `MRSIGNER` is more
stable: it lets policy accept a family of upgraded enclave builds signed by the
same release key, usually combined with `ISVPRODID` and a minimum `ISVSVN`.

## Build And Signing Flow

For a Canopy SGX enclave target, the build creates an enclave library and then
the CMake `enclave_sign(...)` helper signs it. Unless overridden, the signed
output name is:

```text
lib<target>.signed.so
```

Examples:

```bash
cmake --preset Release_SGX
cmake --build build_release_sgx --target <enclave_target>
```

For coroutine SGX:

```bash
cmake --preset Release_Coroutine_SGX
cmake --build build_release_coroutine_sgx --target <enclave_target>
```

For development or simulation builds, use the matching debug/simulation
presets. Do not use simulation or debug enclave identities as production
reference values.

The signing inputs are:

- the unsigned enclave shared object;
- the enclave signing key;
- the SGX enclave config XML;
- the `sgx_sign` tool from the Intel SGX SDK.

The config XML contributes policy-relevant values such as product id, security
version, debug state, heap/stack/TCS sizing, misc-select, and attributes. Treat
the config file as part of the release identity.

## Extracting The Identity

After building the signed enclave, dump the SGX metadata:

```bash
${SGX_SDK}/bin/x64/sgx_sign dump \
  -enclave build_release_sgx/output/lib<target>.signed.so \
  -dumpfile /tmp/<target>.sgx-dump.txt
```

If `SGX_SDK` is not set, Canopy commonly resolves the SDK from `SGX_DIR` or an
installed SDK path such as `/opt/intel/sgxsdk`.

In the dump, record at least:

```text
MRENCLAVE
MRSIGNER
ISVPRODID
ISVSVN
attributes / debug state
misc-select / misc-select-mask when relevant
```

The exact spelling in `sgx_sign dump` output depends on SDK version, but Intel's
own local-attestation examples use this command and look for `mrsigner->value`
in the dump.

For release management, store the approved identity as a machine-readable
manifest rather than copying values into source code by hand. A future Canopy
policy loader should consume this manifest directly.

Example release manifest shape:

```json
{
  "backend_id": "sgx-dcap",
  "policy_name": "treasury-service-production",
  "match": {
    "mrsigner_hex": "hex-encoded-32-bytes",
    "isvprodid": 7,
    "minimum_isvsvn": 12,
    "allow_debug": false,
    "allowed_mrenclave_hex": [
      "optional-exact-release-measurement"
    ]
  }
}
```

Use exact `MRENCLAVE` allow-lists for tightly pinned deployments. Use
`MRSIGNER` plus `ISVPRODID` plus minimum `ISVSVN` when you want controlled
rolling upgrades under the same release signing key.

## How Attestation Matches It

At runtime, Canopy does not trust the dump file. The dump file is only the
developer's reference value.

For SGX DCAP:

1. The peer enclave creates an SGX report with Canopy transcript data in
   `sgx_report_data_t`.
2. The quoting enclave turns that report into a quote.
3. The verifier checks the quote signature, collateral, TCB status, and quote
   freshness.
4. The verifier extracts quote body identity fields such as `MRENCLAVE`,
   `MRSIGNER`, `ISVPRODID`, `ISVSVN`, and attributes.
5. The verifier compares those fields with destination-zone policy.
6. Only then can Canopy mark the route attested and establish protected-RPC
   key material.

For SGX EPID, the same application-level policy comparison applies, but the
quote appraisal path uses the legacy IAS trust anchor. Treat EPID as
compatibility for old SGX1 machines rather than the preferred production path.

## Programmatic Use In Canopy

The SGX EPID and DCAP backend slices already expose verifier seams. Until
Canopy has a first-class measurement-policy object, the real verifier callback
must enforce the release identity allow-list before returning an accepted
verdict.

The intended shape is:

```cpp
struct sgx_release_identity_policy
{
    std::vector<std::array<uint8_t, 32>> allowed_mrenclave;
    std::vector<std::array<uint8_t, 32>> allowed_mrsigner;
    uint16_t isvprodid = 0;
    uint16_t minimum_isvsvn = 0;
    bool allow_debug = false;
};
```

For DCAP, the callback behind `sgx_dcap_host_quote_verifier_options` should:

1. call the Intel quote verifier (`sgx_qv_verify_quote`, `tee_verify_quote`, or
   a QvE/TVL path);
2. reject unacceptable quote-verification results or expired collateral;
3. parse the quote body identity fields;
4. compare those fields with the destination zone's release policy;
5. confirm `sgx_report_data_t` matches the Canopy transcript binding;
6. return accepted only after all checks pass.

Conceptual wiring:

```cpp
sgx_release_identity_policy treasury_policy = load_policy("treasury.json");

sgx_dcap_host_quote_verifier_options verifier_options;
verifier_options.verify_quote =
    [treasury_policy](const sgx_dcap_verifier_input& input,
                      const attestation_policy& policy)
        -> sgx_dcap_host_quote_verification
{
    auto result = run_intel_dcap_verifier(input.quote.quote, policy);
    if (!result.call_succeeded)
        return result;

    auto identity = parse_sgx_quote_identity(input.quote.quote);
    if (!identity_matches(treasury_policy, identity))
    {
        result.call_succeeded = false;
        result.failure_reason = "SGX enclave identity is not accepted by zone policy";
    }
    return result;
};
```

The real implementation should keep parsing and byte comparisons in a small
audited SGX/DCAP module. Do not scatter `MRENCLAVE` or `MRSIGNER` comparison
logic through application code.

## Deterministic Release Builds

Attestation policy is only pleasant to operate if release identities are
repeatable. If the same source and configuration produce a different
`MRENCLAVE` every build, operators have to update allow-lists unnecessarily and
third parties cannot reproduce the enclave identity.

Intel's SGX reproducibility notes describe three requirements:

- stable source code;
- a clean and secure build environment;
- an auditable build toolchain.

Their supplied SGX reproducibility tooling uses Docker plus Nix to make the
toolchain reproducible. Canopy should follow the same model for formal enclave
releases.

Recommended Canopy procedure:

1. Build release enclaves from a clean checkout at a recorded commit.
2. Record every submodule revision.
3. Use a fixed CMake preset, normally `Release_SGX` or
   `Release_Coroutine_SGX`.
4. Use a fixed compiler, SGX SDK, SGXSSL/DCAP dependency set, CMake version,
   generator, and build container image.
5. Use a fixed enclave config XML and signing key.
6. Avoid build-time entropy in enclave code: timestamps, random generated
   headers, host paths, usernames, temporary directory names, or non-stable
   generated source order.
7. Leave `CANOPY_SGX_DETERMINISTIC_BUILD=ON` for release enclave builds. This
   makes `cmake/SGX.cmake` apply compiler path-prefix maps, reject timestamp
   macros through `-Wdate-time`, and pass deterministic environment variables
   to SGX helper tools.
8. Set either the environment variable `SOURCE_DATE_EPOCH` or the CMake cache
   variable `CANOPY_SGX_SOURCE_DATE_EPOCH` to the release timestamp that should
   represent the build.
9. After signing, run `sgx_sign dump` and archive the dump, signed enclave,
   unsigned enclave, config XML, public signing key, CMake cache, and build
   manifest.
10. Compare release identity fields with the previous expected manifest before
   publishing.

For reproducibility checks, do not rely only on a byte-for-byte comparison of
the signed enclave file. SGX signing metadata can contain fields that vary
between signing runs or between signers. Intel's AE reproducibility verifier
normalizes `sgx_sign dump` output and compares the policy-relevant metadata
while excluding fields such as signing date and signer-specific signature
material.

For Canopy releases, the first useful reproducibility gate is:

```text
same source + same toolchain + same config -> same MRENCLAVE
same signing key                         -> same MRSIGNER
same config                              -> same ISVPRODID / ISVSVN / DEBUG policy
```

If `MRENCLAVE` changes unexpectedly, treat it as a release blocker until the
change is explained by a deliberate code, dependency, compiler, linker, or
config change.

## Operational Rules

- Never approve a production peer solely because it is "an SGX enclave".
- Reject debug enclaves unless the destination zone explicitly allows debug
  peers.
- Prefer DCAP for modern SGX production.
- Treat EPID as legacy compatibility.
- Keep release signing keys separate from development keys.
- Rotate `ISVSVN` upward for security fixes and require a minimum `ISVSVN` in
  policy.
- Keep exact `MRENCLAVE` pins for high-value services or staged rollouts.
- Keep `MRSIGNER`-based policy for services that need rolling upgrades under a
  trusted release key.
- Make destination-zone policy the final decision. A gateway zone may accept
  unattested web clients while a treasury zone requires bidirectional hardware
  attestation.

## See Also

- [Attestation Backends](attestation-backends.md)
- [DCAP Operations](dcap-operations.md)
- [Implementation Plan](implementation-plan.md)
- Intel SGX reproducibility tooling in
  `submodules/confidential-computing.sgx/linux/reproducibility/`
- Canopy SGX signing helper in `cmake/FindSGX.cmake`
