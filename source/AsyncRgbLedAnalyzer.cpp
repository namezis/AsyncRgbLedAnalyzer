#include "AsyncRgbLedAnalyzer.h"
#include "AsyncRgbLedAnalyzerSettings.h"
#include "AsyncRgbLedAnalyzerResults.h"

#include <AnalyzerChannelData.h>

#include <iostream>
#include <algorithm> // for std::max/max()

//#define LED_LOGGING

AsyncRgbLedAnalyzer::AsyncRgbLedAnalyzer()
    :   Analyzer2(),
        mSettings( new AsyncRgbLedAnalyzerSettings )
{
    SetAnalyzerSettings( mSettings.get() );
}

AsyncRgbLedAnalyzer::~AsyncRgbLedAnalyzer()
{
    KillThread();
}

void AsyncRgbLedAnalyzer::SetupResults()
{
    mResults.reset( new AsyncRgbLedAnalyzerResults( this, mSettings.get() ) );
    SetAnalyzerResults( mResults.get() );
    mResults->AddChannelBubblesWillAppearOn( mSettings->mInputChannel );
}

void AsyncRgbLedAnalyzer::WorkerThread()
{
    mSampleRateHz = GetSampleRate();
    mHalfSampleWidth = 0.5 / mSampleRateHz;
    mChannelData = GetAnalyzerChannelData( mSettings->mInputChannel );

    // cache this value here to avoid recomputing this every bit-read
    if ( mSettings->IsHighSpeedSupported() )
    {
        mMinimumLowDurationSec = std::min( mSettings->DataTiming( BIT_LOW, true ).mNegativeTiming.mMinimumSec,
                                           mSettings->DataTiming( BIT_HIGH, true ).mNegativeTiming.mMinimumSec );
    }
    else
    {
        mMinimumLowDurationSec = std::min( mSettings->DataTiming( BIT_LOW ).mNegativeTiming.mMinimumSec,
                                           mSettings->DataTiming( BIT_HIGH ).mNegativeTiming.mMinimumSec );
    }

    mMinimumLowDurationSec-= mHalfSampleWidth;

    bool isResyncNeeded = true;

    for ( ; ; )
    {
        if ( isResyncNeeded )
        {
            SynchronizeToReset();
            isResyncNeeded = false;
        }

        mFirstBitAfterReset = true;
        U32 frameInPacketIndex = 0;
        mResults->CommitPacketAndStartNewPacket();

        // data word reading loop
        for ( ; ; )
        {
            auto result = ReadRGBTriple();

            if ( result.mValid )
            {
                Frame frame;
                frame.mFlags = 0;
                frame.mStartingSampleInclusive = result.mValueBeginSample;
                frame.mEndingSampleInclusive = result.mValueEndSample;
                frame.mData1 = result.mRGB.ConvertToU64();
                frame.mData2 = frameInPacketIndex++;
                mResults->AddFrame( frame );
                mResults->CommitResults();
            }
            else
            {
#if defined(LED_LOGGING)
                std::cerr << "failed to read frame at:" << (frameInPacketIndex) << std::endl;
#endif
                // something error occurred, let's resynchronise
                isResyncNeeded = true;
            }

            if ( isResyncNeeded || result.mIsReset )
            {
                break;
            }
        }

        mResults->CommitResults();
        ReportProgress( mChannelData->GetSampleNumber() );
    }
}

void AsyncRgbLedAnalyzer::SynchronizeToReset()
{
    if ( mChannelData->GetBitState() == BIT_HIGH )
    {
        mChannelData->AdvanceToNextEdge();
    }

    for ( ; ; )
    {
        const U64 lowTransition = mChannelData->GetSampleNumber();
        const U64 highTransition = mChannelData->GetSampleOfNextEdge();
        double lowTimeSec = ( highTransition - lowTransition ) / mSampleRateHz;

        if ( lowTimeSec > (mSettings->ResetTiming().mMinimumSec - mHalfSampleWidth))
        {
            // it's a reset, we are done
            // advance to the end of the reset, ready for the first
            // ReadRGB / ReadBit
            mChannelData->AdvanceToAbsPosition( highTransition );
            return;
        }

        // advance past the rising edge, to the next falling edge,
        // which is our next candidate for the beginning of a RESET
        mChannelData->AdvanceToAbsPosition( highTransition );
        mChannelData->AdvanceToNextEdge();
    }
}

auto AsyncRgbLedAnalyzer::ReadRGBTriple() -> RGBResult
{
    const U8 bitSize =  mSettings->BitSize();
    U16 channels[3] = {0, 0, 0};
    RGBResult result;

    DataBuilder builder;
    int channel = 0;

    for ( ; channel < 3; )
{
U64 value = 0;
builder.Reset( &value, AnalyzerEnums::MsbFirst, bitSize );
    int i = 0;

    for ( ; i < bitSize; ++i )
    {
        auto bitResult = ReadBit();

        if ( !bitResult.mValid )
        {
#if defined(LED_LOGGING)
            std::cerr << "RGB read failure at bit " << i << std::endl;
#endif
            break;
        }

        // for the first bit of channel 0, record the beginning time
        // for accurate frame positions in the results
        if ( ( i == 0 ) && ( channel == 0 ) )
        {
            result.mValueBeginSample = bitResult.mBeginSample;
        }

        result.mValueEndSample = bitResult.mEndSample;
        builder.AddBit( bitResult.mBitValue );
        result.mIsReset = bitResult.mIsReset;
    }

    if ( i == bitSize )
    {
        // we saw a complete channel, save it
        channels[channel++] = value;
    }
    else
    {
        // partial data due to reset or invalid timing, discard
        break;
    }
}

if ( channel == 3 )
{
// we saw three complete channels, we can use this
result.mRGB = RGBValue::CreateFromControllerOrder( mSettings->GetColorLayout(), channels );
    result.mValid = true;
} // in all other cases, mValid stays false - no RGB data was written

return result;
}

auto AsyncRgbLedAnalyzer::ReadBit() -> ReadResult
{
    ReadResult result;
    result.mValid = false;

    if ( mChannelData->GetBitState() == BIT_LOW )
    {
        mChannelData->AdvanceToNextEdge();
    }

    result.mBeginSample = mChannelData->GetSampleNumber();
    mChannelData->AdvanceToNextEdge();
    const U64 fallingEdgeSample = mChannelData->GetSampleNumber();
    const double highTimeSec = ( fallingEdgeSample - result.mBeginSample ) / mSampleRateHz;

 //   std::cout << "hi time:" << highTimeSec << " (" << ( fallingEdgeSample - result.mBeginSample ) << ")" << std::endl;
    if ( mFirstBitAfterReset )
    {
        // we can't classify yet, need to wait until we have the low pulse timing
    }
    else
    {
        // clasify based on existing value
        // ensure consistency with previously detected speed setting
        if ( mSettings->DataTiming( BIT_LOW, mDidDetectHighSpeed ).mPositiveTiming.WithinTolerance( highTimeSec, mHalfSampleWidth ) )
        {
            result.mBitValue = BIT_LOW;
        }
        else if ( mSettings->DataTiming( BIT_HIGH, mDidDetectHighSpeed ).mPositiveTiming.WithinTolerance( highTimeSec, mHalfSampleWidth ) )
        {
            result.mBitValue = BIT_HIGH;
        }
        else
        {
#if defined(LED_LOGGING)
            std::cerr << "positive pulse timing doesn't match detected speed mode" << std::endl;
            std::cerr << "\tdetected: " << (mDidDetectHighSpeed ? "Hi-speed" : "Normal") << std::endl;
            std::cerr << "\t" << highTimeSec << std::endl;
#endif
            mChannelData->AdvanceToAbsPosition( fallingEdgeSample );
            return result; // invalid result, reset required
        }
    }

    // check for a too-short low timing
    if ( mChannelData->WouldAdvancingCauseTransition( mMinimumLowDurationSec * mSampleRateHz ) )
    {   
        mChannelData->AdvanceToNextEdge();
#if defined(LED_LOGGING)
        const double lowTimeSec = ( mChannelData->GetSampleNumber() - fallingEdgeSample ) / mSampleRateHz;
        std::cerr << "too short low pulse, invalid bit" << std::endl;
        std::cerr << "\t" << lowTimeSec << std::endl;
#endif
        return result; // invalid result, reset required
    }

    // check for a low period exceeding the minimum reset time
    // if we exceed that, this is a reset
    const int minResetSamples = static_cast<int>( mSettings->ResetTiming().mMinimumSec * mSampleRateHz );

    if ( !mChannelData->WouldAdvancingCauseTransition( minResetSamples ) )
    {
        // if we see a single bit in between resets, we can't decode the speed,
        // but this is meaningless anyway, so return an error
        if ( mFirstBitAfterReset )
        {
#if defined(LED_LOGGING)
            std::cerr << "No complete bit between resets, can't decode" << std::endl;
#endif
            return result; // return invalid
        }

        mChannelData->Advance( minResetSamples );
        result.mIsReset = true;
    }
    else
    {
        // we saw a transition, let's see the timing
        mChannelData->AdvanceToNextEdge();

        // the -1 is so the end of this frame, and start of the next, don't
        // overlap.
        result.mEndSample = mChannelData->GetSampleNumber() - 1;
    }

    if ( result.mIsReset )
    {
        // if this bit is also a reset, we can't check the low time since it
        // will exceed the maximums, but we still want to accept that case
        // as valid
        result.mValid = true;

        // use the nominal negative pulse timing for the frame ending.
        double nominalNegativeSec = mSettings->DataTiming( result.mBitValue, mDidDetectHighSpeed ).mNegativeTiming.mNominalSec;
        result.mEndSample = fallingEdgeSample + ( nominalNegativeSec * mSampleRateHz );
    }
    else if ( mFirstBitAfterReset )
    {
        const double lowTimeSec = ( result.mEndSample - fallingEdgeSample ) / mSampleRateHz;
        // two-way classification. This is necessary because the the 0-data
        // positive pulse of low-speed mode can match the 1-data positive pulse
        // in high speed mode, for some controllers. Hence we need to correlate
        // the high and low times to detect the speed mode

        // this also sets mBitValue correct as a side-effect of the detection
        result.mValid = DetectSpeedMode( highTimeSec, lowTimeSec, result.mBitValue );
    }
    else
    {
        // already detected the speed mode, ensure consistency
        const double lowTimeSec = ( result.mEndSample - fallingEdgeSample ) / mSampleRateHz;
    //    std::cout << "low time:" << lowTimeSec << " (" << (result.mEndSample - fallingEdgeSample  ) << ")" << std::endl;

        if ( mSettings->DataTiming( result.mBitValue, mDidDetectHighSpeed ).mNegativeTiming.WithinTolerance( lowTimeSec, mHalfSampleWidth ) )
        {
            // we are good
            result.mValid = true;
        }
        else
        {
#if defined(LED_LOGGING)
            // we could do further classification here on the error, eg speed mismatch,
            // or bit value mismatch
            std::cerr << "negative pulse timing doesn't match positive pulse" << std::endl;
            std::cerr << "\tdetected: " << (mDidDetectHighSpeed ? "Hi-speed" : "Normal") << std::endl;
            std::cerr << "\t" << highTimeSec <<  " / " << lowTimeSec << std::endl;
            std::cerr << "\texpected:" << mSettings->DataTiming( result.mBitValue, mDidDetectHighSpeed ) << std::endl;
#endif
            result.mValid = false;
        }
    }

    return result;
}

bool AsyncRgbLedAnalyzer::DetectSpeedMode( double positiveTimeSec, double negativeTimeSec, BitState& value )
{
    const double halfSampleWidth = 0.5 / mSampleRateHz;
    mDidDetectHighSpeed = false;

    // low speed bits
    for ( const auto b : {BIT_LOW, BIT_HIGH} )
    {
        if ( mSettings->DataTiming( b ).WithinTolerance( positiveTimeSec, negativeTimeSec, halfSampleWidth ) )
        {
            value = b;
            mFirstBitAfterReset = false;
            return true;
        }
    }

    if ( mSettings->IsHighSpeedSupported() )
    {
        // high speed bits
        for ( const auto b : {BIT_LOW, BIT_HIGH} )
        {
            if ( mSettings->DataTiming( b, true ).WithinTolerance( positiveTimeSec, negativeTimeSec, halfSampleWidth ) )
            {
                mDidDetectHighSpeed = true;
                value = b;
                mFirstBitAfterReset = false;
                return true;
            }
        }
    } // of high-speed mode tests

    std::cerr << "failed to classify: " << positiveTimeSec << "/" << negativeTimeSec << std::endl;
    return false;
}

bool AsyncRgbLedAnalyzer::NeedsRerun()
{
    return false;
}

U32 AsyncRgbLedAnalyzer::GenerateSimulationData( U64 minimum_sample_index, U32 device_sample_rate, SimulationChannelDescriptor** simulation_channels )
{
    if ( mSimulationInitialized == false )
    {
        mSimulationDataGenerator.Initialize( GetSimulationSampleRate(), mSettings.get() );
        mSimulationInitialized = true;
    }

    return mSimulationDataGenerator.GenerateSimulationData( minimum_sample_index, device_sample_rate, simulation_channels );
}

U32 AsyncRgbLedAnalyzer::GetMinimumSampleRateHz()
{
    return 12 * 100000; // 12Mhz minimum sample rate
}

const char* AsyncRgbLedAnalyzer::GetAnalyzerName() const
{
    return "Addressable LEDs (Async)";
}

const char* GetAnalyzerName()
{
    return "Addressable LEDs (Async)";
}

Analyzer* CreateAnalyzer()
{
    return new AsyncRgbLedAnalyzer;
}

void DestroyAnalyzer( Analyzer* analyzer )
{
    delete analyzer;
}
