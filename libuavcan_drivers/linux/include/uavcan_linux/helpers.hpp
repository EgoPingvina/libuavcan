/*
 * Copyright (C) 2014 Pavel Kirienko <pavel.kirienko@gmail.com>
 */

#pragma once

#include <memory>
#include <string>
#include <vector>
#include <chrono>
#include <iostream>
#include <sstream>
#include <uavcan/uavcan.hpp>

namespace uavcan_linux
{
/**
 * Default log sink will dump everything into stderr
 */
class DefaultLogSink : public uavcan::ILogSink
{
    virtual void log(const uavcan::protocol::debug::LogMessage& message)
    {
        const auto tt = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        const auto tstr = std::ctime(&tt);
        std::cerr << "### UAVCAN " << tstr << message << std::endl;
    }
};

/**
 * Contains all drivers needed for uavcan::Node.
 */
struct DriverPack
{
    SystemClock clock;
    SocketCanDriver can;

    explicit DriverPack(ClockAdjustmentMode clock_adjustment_mode)
        : clock(clock_adjustment_mode)
        , can(clock)
    { }
};

typedef std::shared_ptr<DriverPack> DriverPackPtr;

typedef std::shared_ptr<uavcan::Timer> TimerPtr;

static constexpr std::size_t NodeMemPoolSize = 1024 * 512;  // One size fits all

/**
 * Wrapper for uavcan::Node with some additional convenience functions.
 */
class Node : public uavcan::Node<NodeMemPoolSize>
{
    DriverPackPtr driver_pack_;
    DefaultLogSink log_sink_;

    static void enforce(int error, const std::string& msg)
    {
        if (error < 0)
        {
            std::ostringstream os;
            os << msg << " [" << error << "]";
            throw Exception(os.str());
        }
    }

    template <typename DataType>
    static std::string getDataTypeName()
    {
        return DataType::getDataTypeFullName();
    }

public:
    /**
     * Simple forwarding constructor, compatible with uavcan::Node
     */
    Node(uavcan::ICanDriver& can_driver, uavcan::ISystemClock& clock)
        : uavcan::Node<NodeMemPoolSize>(can_driver, clock)
    {
        getLogger().setExternalSink(&log_sink_);
    }

    /**
     * Takes ownership of the driver container.
     */
    explicit Node(DriverPackPtr driver_pack)
        : uavcan::Node<NodeMemPoolSize>(driver_pack->can, driver_pack->clock)
        , driver_pack_(driver_pack)
    {
        getLogger().setExternalSink(&log_sink_);
    }

    template <typename DataType>
    std::shared_ptr<uavcan::Subscriber<DataType>>
    makeSubscriber(const typename uavcan::Subscriber<DataType>::Callback& cb)
    {
        std::shared_ptr<uavcan::Subscriber<DataType>> p(new uavcan::Subscriber<DataType>(*this));
        enforce(p->start(cb), "Subscriber start failure " + getDataTypeName<DataType>());
        return p;
    }

    template <typename DataType>
    std::shared_ptr<uavcan::Publisher<DataType>>
    makePublisher(uavcan::MonotonicDuration tx_timeout = uavcan::Publisher<DataType>::getDefaultTxTimeout())
    {
        std::shared_ptr<uavcan::Publisher<DataType>> p(new uavcan::Publisher<DataType>(*this));
        enforce(p->init(), "Publisher init failure " + getDataTypeName<DataType>());
        p->setTxTimeout(tx_timeout);
        return p;
    }

    template <typename DataType>
    std::shared_ptr<uavcan::ServiceServer<DataType>>
    makeServiceServer(const typename uavcan::ServiceServer<DataType>::Callback& cb)
    {
        std::shared_ptr<uavcan::ServiceServer<DataType>> p(new uavcan::ServiceServer<DataType>(*this));
        enforce(p->start(cb), "ServiceServer start failure " + getDataTypeName<DataType>());
        return p;
    }

    template <typename DataType>
    std::shared_ptr<uavcan::ServiceClient<DataType>>
    makeServiceClient(const typename uavcan::ServiceClient<DataType>::Callback& cb)
    {
        std::shared_ptr<uavcan::ServiceClient<DataType>> p(new uavcan::ServiceClient<DataType>(*this));
        enforce(p->init(), "ServiceClient init failure " + getDataTypeName<DataType>());
        p->setCallback(cb);
        return p;
    }

    TimerPtr makeTimer(uavcan::MonotonicTime deadline, const typename uavcan::Timer::Callback& cb)
    {
        TimerPtr p(new uavcan::Timer(*this));
        p->setCallback(cb);
        p->startOneShotWithDeadline(deadline);
        return p;
    }

    TimerPtr makeTimer(uavcan::MonotonicDuration period, const typename uavcan::Timer::Callback& cb)
    {
        TimerPtr p(new uavcan::Timer(*this));
        p->setCallback(cb);
        p->startPeriodic(period);
        return p;
    }
};

typedef std::shared_ptr<Node> NodePtr;

/**
 * Constructs Node with explicitly specified ClockAdjustmentMode.
 */
static inline NodePtr makeNode(const std::vector<std::string>& iface_names, ClockAdjustmentMode clock_adjustment_mode)
{
    DriverPackPtr dp(new DriverPack(clock_adjustment_mode));
    for (auto ifn : iface_names)
    {
        if (dp->can.addIface(ifn) < 0)
        {
            throw Exception("Failed to add iface " + ifn);
        }
    }
    return NodePtr(new Node(dp));
}

/**
 * This is the preferred way to make Node.
 */
static inline NodePtr makeNode(const std::vector<std::string>& iface_names)
{
    return makeNode(iface_names, SystemClock::detectPreferredClockAdjustmentMode());
}

/**
 * Wrapper over uavcan::ServiceClient<> for blocking calls.
 * Calls spin() internally.
 */
template <typename DataType>
class BlockingServiceClient : public uavcan::ServiceClient<DataType>
{
    typedef uavcan::ServiceClient<DataType> Super;

    typename DataType::Response response_;
    bool call_was_successful_;

    void callback(const uavcan::ServiceCallResult<DataType>& res)
    {
        response_ = res.response;
        call_was_successful_ = res.isSuccessful();
    }

    void setup()
    {
        Super::setCallback(std::bind(&BlockingServiceClient::callback, this, std::placeholders::_1));
        call_was_successful_ = false;
        response_ = typename DataType::Response();
    }

public:
    BlockingServiceClient(uavcan::INode& node)
        : uavcan::ServiceClient<DataType>(node)
        , call_was_successful_(false)
    {
        setup();
    }

    int blockingCall(uavcan::NodeID server_node_id, const typename DataType::Request& request)
    {
        const auto SpinDuration = uavcan::MonotonicDuration::fromMSec(2);
        setup();
        const int call_res = Super::call(server_node_id, request);
        if (call_res >= 0)
        {
            while (Super::isPending())
            {
                const int spin_res = Super::getNode().spin(SpinDuration);
                if (spin_res < 0)
                {
                    return spin_res;
                }
            }
        }
        return call_res;
    }

    int blockingCall(uavcan::NodeID server_node_id, const typename DataType::Request& request,
                     uavcan::MonotonicDuration timeout)
    {
        Super::setRequestTimeout(timeout);
        return blockingCall(server_node_id, request);
    }

    bool wasSuccessful() const { return call_was_successful_; }

    const typename DataType::Response& getResponse() const { return response_; }
};

}
