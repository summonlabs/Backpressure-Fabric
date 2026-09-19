#pragma once

// Backpressure Fabric - umbrella header.
//
// Vendor-neutral C++20 runtime for generation-bound propagation of network
// resource pressure with bounded scope, damping, loop prevention, authority and
// recovery semantics.
//
// The fabric owns pressure-propagation authority, scope, damping, lineage and
// loop prevention. It does not own congestion detection, queue or buffer
// implementation, rate enforcement, packet pacing, admission, path computation,
// flow scheduling, generic credit semantics or congestion recovery sequencing.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "backpressure/version.hpp"

#include "backpressure/core/byteio.hpp"
#include "backpressure/core/checked.hpp"
#include "backpressure/core/clock.hpp"
#include "backpressure/core/digest.hpp"
#include "backpressure/core/fixed.hpp"
#include "backpressure/core/ids.hpp"
#include "backpressure/core/rng.hpp"
#include "backpressure/core/status.hpp"

#include "backpressure/model/authority.hpp"
#include "backpressure/model/dependency.hpp"
#include "backpressure/model/fence.hpp"
#include "backpressure/model/generation.hpp"
#include "backpressure/model/observation.hpp"
#include "backpressure/model/policy.hpp"
#include "backpressure/model/resource.hpp"
#include "backpressure/model/signal.hpp"
#include "backpressure/model/topology.hpp"

#include "backpressure/engine/engine.hpp"
#include "backpressure/engine/explain.hpp"

#include "backpressure/store/durable_state.hpp"
#include "backpressure/store/fsutil.hpp"
#include "backpressure/store/journal.hpp"

#include "backpressure/ipc/frame.hpp"
#include "backpressure/ipc/subprocess.hpp"

#include "backpressure/runtime/fabric.hpp"

#include "backpressure/bench/synthetic.hpp"
