/*
 *
 * Copyright 2018 Asylo authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include "asylo/identity/sgx/sgx_local_assertion_generator.h"

#include <string>

#include "absl/synchronization/mutex.h"
#include "asylo/crypto/sha256_hash.h"
#include "asylo/crypto/util/bytes.h"
#include "asylo/crypto/util/trivial_object_util.h"
#include "asylo/identity/sgx/code_identity_constants.h"
#include "asylo/identity/sgx/hardware_interface.h"
#include "asylo/identity/sgx/identity_key_management_structs.h"
#include "asylo/identity/sgx/local_assertion.pb.h"
#include "asylo/platform/core/trusted_global_state.h"

namespace asylo {

const char *const SgxLocalAssertionGenerator::authority_type_ =
    sgx::kSgxLocalAssertionAuthority;

SgxLocalAssertionGenerator::SgxLocalAssertionGenerator()
    : initialized_(false) {}

Status SgxLocalAssertionGenerator::Initialize(const std::string &config) {
  if (IsInitialized()) {
    return Status(error::GoogleError::FAILED_PRECONDITION,
                  "Already initialized");
  }

  auto config_result = GetEnclaveConfig();
  if (!config_result.ok()) {
    return config_result.status();
  }

  const EnclaveConfig *enclave_config = config_result.ValueOrDie();
  if (!enclave_config->host_config().has_local_attestation_domain()) {
    return Status(error::GoogleError::INTERNAL,
                  "Config is missing attestation domain");
  }

  attestation_domain_ =
      enclave_config->host_config().local_attestation_domain();

  absl::MutexLock lock(&initialized_mu_);
  initialized_ = true;

  return Status::OkStatus();
}

bool SgxLocalAssertionGenerator::IsInitialized() const {
  absl::MutexLock lock(&initialized_mu_);
  return initialized_;
}

EnclaveIdentityType SgxLocalAssertionGenerator::IdentityType() const {
  return identity_type_;
}

std::string SgxLocalAssertionGenerator::AuthorityType() const {
  return authority_type_;
}

Status SgxLocalAssertionGenerator::CreateAssertionOffer(
    AssertionOffer *offer) const {
  if (!IsInitialized()) {
    return Status(error::GoogleError::FAILED_PRECONDITION, "Not initialized");
  }

  offer->mutable_description()->set_identity_type(IdentityType());
  offer->mutable_description()->set_authority_type(AuthorityType());

  sgx::LocalAssertionOfferAdditionalInfo additional_info;
  additional_info.set_local_attestation_domain(attestation_domain_);
  if (!additional_info.SerializeToString(
          offer->mutable_additional_information())) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to serialize LocalAssertionOfferAdditionalInfo");
  }

  return Status::OkStatus();
}

StatusOr<bool> SgxLocalAssertionGenerator::CanGenerate(
    const AssertionRequest &request) const {
  if (!IsInitialized()) {
    return Status(error::GoogleError::FAILED_PRECONDITION, "Not initialized");
  }

  StatusOr<sgx::LocalAssertionRequestAdditionalInfo> additional_info_result =
      ParseAdditionalInfo(request);

  if (!additional_info_result.ok()) {
    return additional_info_result.status();
  }

  sgx::LocalAssertionRequestAdditionalInfo additional_info =
      additional_info_result.ValueOrDie();

  return additional_info.local_attestation_domain() == attestation_domain_;
}

Status SgxLocalAssertionGenerator::Generate(const std::string &user_data,
                                            const AssertionRequest &request,
                                            Assertion *assertion) const {
  if (!IsInitialized()) {
    return Status(error::GoogleError::FAILED_PRECONDITION, "Not initialized");
  }

  StatusOr<sgx::LocalAssertionRequestAdditionalInfo> additional_info_result =
      ParseAdditionalInfo(request);
  if (!additional_info_result.ok()) {
    return additional_info_result.status();
  }

  sgx::LocalAssertionRequestAdditionalInfo additional_info =
      additional_info_result.ValueOrDie();

  if (additional_info.local_attestation_domain() != attestation_domain_) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "AssertionRequest specifies non-local attestation domain");
  }

  sgx::AlignedTargetinfoPtr tinfo;
  if (additional_info.targetinfo().size() != sizeof(*tinfo)) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "TARGETINFO from AssertionRequest has incorrect size");
  }

  // The layout and endianness of the TARGETINFO structure is defined by the
  // Intel SGX architecture. Consequently, two SGX-enabled machines can use a
  // common wire-format for this structure by simply dumping the raw bytes of
  // this structure into a proto. Here, we assume that the TARGETINFO string in
  // the request originates from a machine that supports the Intel SGX
  // architecture, and was copied into the request byte-for-byte. Since the
  // LocalAssertionGenerator runs inside an SGX enclave, it is safe to restore
  // the TARGETINFO structure directly from the request.
  *tinfo = TrivialObjectFromBinaryString<sgx::Targetinfo>(
      additional_info.targetinfo());

  // The REPORTDATA is a user-provided input to the hardware report that is
  // included in the report's MAC. Use a SHA256 hash of |user_data| as the
  // REPORTDATA value so that the resulting assertion is cryptographically-bound
  // to this user-provided data. Note that the SHA256 hash only occupies the
  // lower 32 bytes of the 64-byte REPORTDATA structure so the structure is
  // pre-filled with an additional 32 zeros.
  sgx::AlignedReportdataPtr reportdata;
  Sha256Hash hash;
  hash.Update(user_data.data(), user_data.size());
  reportdata->data = TrivialZeroObject<UnsafeBytes<sgx::kReportdataSize>>();
  reportdata->data.replace(/*pos=*/0, hash.CumulativeHash());

  // Generate a REPORT that is bound to the provided |user_data| and is targeted
  // at the enclave described in the request.
  sgx::AlignedReportPtr report;
  if (!sgx::GetHardwareReport(*tinfo, *reportdata, report.get())) {
    return Status(error::GoogleError::INTERNAL, "Failed to generate a REPORT");
  }

  // As explained above, the REPORT structure can be copied byte-for-byte into
  // the report field of the assertion because the layout and endianness of the
  // structure is defined by the Intel SGX architecture. As a result, dumping
  // the raw bytes of the report is sufficient when the structure is sent
  // between two SGX-enabled machines. An SGX-enabled assertion verifier should
  // be able to restore these bytes into a valid REPORT structure.
  sgx::LocalAssertion local_assertion;
  local_assertion.set_report(reinterpret_cast<const char *>(report.get()),
                             sizeof(*report));

  if (!local_assertion.SerializeToString(assertion->mutable_assertion())) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to serialize local assertion");
  }
  assertion->mutable_description()->set_identity_type(IdentityType());
  assertion->mutable_description()->set_authority_type(AuthorityType());

  return Status::OkStatus();
}

StatusOr<sgx::LocalAssertionRequestAdditionalInfo>
SgxLocalAssertionGenerator::ParseAdditionalInfo(
    const AssertionRequest &request) const {
  if (!IsCompatibleAssertionDescription(request.description())) {
    return Status(error::GoogleError::INVALID_ARGUMENT,
                  "Incompatible assertion description");
  }

  sgx::LocalAssertionRequestAdditionalInfo additional_info;
  if (!additional_info.ParseFromString(request.additional_information())) {
    return Status(error::GoogleError::INTERNAL,
                  "Failed to parse request additional information");
  }

  return additional_info;
}

// Static registration of the LocalAssertionGenerator library.
SET_STATIC_MAP_VALUE_OF_DERIVED_TYPE(AssertionGeneratorMap,
                                     SgxLocalAssertionGenerator);

}  // namespace asylo
