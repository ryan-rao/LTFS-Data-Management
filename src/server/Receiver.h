#pragma once

extern std::atomic<long> globalReqNumber;

class Receiver

{
public:
    Receiver()
    {
    }
    ~Receiver()
    {
    }
    void run(long key, std::shared_ptr<Connector> connector);
};
