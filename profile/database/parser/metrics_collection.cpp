// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved

#include "metrics_collection.h"

namespace xdp {
// MetricCollection class for managing a collection of metrics

    void
    MetricCollection::addMetric(std::unique_ptr<Metric> metric) {
        if (metric) {
            metrics.push_back(std::move(metric));
        } else {
            xrt_core::message::send(severity_level::debug, "XRT", "Null metric cannot be added to collection");
        }
    }

    bool
    MetricCollection::hasAllTileRanges() const {
        // Check if all tile ranges are present in the collection
        for (const auto& metric : metrics) {
            if (metric && metric->isAllTilesSet()) {
                xrt_core::message::send(severity_level::debug, "XRT", "Metric \"all\" tile range found");
                return true;
            }
        }
        return false;
    }

    bool
    MetricCollection::hasIndividualTiles() const {
        // Check if the collection contains a metric with all tiles range
        for (const auto& metric : metrics) {
            if (metric && !metric->isAllTilesSet()) {
                return true;
            }
        }
        xrt_core::message::send(severity_level::debug, "XRT", "Metric all individual tiles found");
        return false;
    }

    // Convert to ptree array
    boost::property_tree::ptree
    MetricCollection::toPtree() const {
        boost::property_tree::ptree arr;
        for (const auto& metric : metrics) {
            metric->print();
            boost::property_tree::ptree obj = metric->toPtree();
            arr.push_back(std::make_pair("", obj));
        }
        return arr;
    }

    void
    MetricCollection::print() const {
        // std::cout << "!!! Print MetricCollection:" << std::endl;
        for (const auto& metric : metrics) {
            if (metric) {
                metric->print();
            } else {
                xrt_core::message::send(severity_level::warning, "XRT", "Null metric found in collection");
            }
        }
    }
}; // namespace xdp