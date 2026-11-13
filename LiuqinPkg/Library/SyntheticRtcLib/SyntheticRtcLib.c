/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <Uefi.h>
#include <Library/RealTimeClockLib.h>
#include <Library/TimerLib.h>

#ifndef BUILD_EPOCH
#error BUILD_EPOCH must be supplied from SOURCE_DATE_EPOCH
#endif

#define RTC_NANOSECONDS 1000000000U
#define RTC_SECONDS_PER_DAY 86400ULL
#define RTC_DAYS_BEFORE_1900 693595ULL
#define RTC_1970_FROM_1900 2208988800LL
#define RTC_LAST_SECOND 255611289599LL

typedef struct
{
  BOOLEAN Ready;
  UINT64 Frequency;
  UINT64 CounterStart;
  UINT64 CounterEnd;
  UINT64 Anchor;
  INT64 LocalSeconds;
  UINT32 Nanosecond;
  INT16 TimeZone;
  UINT8 Daylight;
} SYNTHETIC_RTC_STATE;

STATIC SYNTHETIC_RTC_STATE mRtc;
STATIC CONST UINT8 mMonthDays[12] = {31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U};

STATIC
BOOLEAN
LeapYear (UINT32 Year)
{
  return (BOOLEAN)(((Year % 4U) == 0U) && (((Year % 100U) != 0U) || ((Year % 400U) == 0U)));
}

STATIC
UINT8
MonthDays (UINT32 Year, UINT8 Month)
{
  return (UINT8)(mMonthDays[Month - 1U] + (((Month == 2U) && LeapYear (Year)) ? 1U : 0U));
}

STATIC
BOOLEAN
ValidTime (CONST EFI_TIME *Time)
{
  if ((Time == NULL) || (Time->Year < 1900U) || (Time->Year > 9999U) || (Time->Month < 1U) ||
      (Time->Month > 12U) || (Time->Day < 1U) || (Time->Hour > 23U) || (Time->Minute > 59U) ||
      (Time->Second > 59U) || (Time->Nanosecond >= RTC_NANOSECONDS) ||
      ((Time->TimeZone != EFI_UNSPECIFIED_TIMEZONE) &&
       ((Time->TimeZone < -1440) || (Time->TimeZone > 1440))) ||
      ((Time->Daylight & ~(EFI_TIME_ADJUST_DAYLIGHT | EFI_TIME_IN_DAYLIGHT)) != 0U))
  {
    return FALSE;
  }

  return (BOOLEAN)(Time->Day <= MonthDays (Time->Year, Time->Month));
}

STATIC
UINT64
DaysBeforeYear (UINT32 Year)
{
  UINT64 Previous;

  Previous = (UINT64)Year - 1ULL;
  return (Previous * 365ULL) + (Previous / 4ULL) - (Previous / 100ULL) + (Previous / 400ULL);
}

STATIC
INT64
CalendarSeconds (CONST EFI_TIME *Time)
{
  UINT64 Days;
  UINT8 Month;

  Days = DaysBeforeYear (Time->Year) - RTC_DAYS_BEFORE_1900;
  for (Month = 1U; Month < Time->Month; ++Month)
  {
    Days += MonthDays (Time->Year, Month);
  }

  Days += (UINT64)Time->Day - 1ULL;
  return (INT64)((Days * RTC_SECONDS_PER_DAY) + ((UINT64)Time->Hour * 3600ULL) +
                 ((UINT64)Time->Minute * 60ULL) + Time->Second);
}

STATIC
VOID SecondsToCalendar (UINT64 Seconds, EFI_TIME *Time)
{
  UINT64 Days;
  UINT32 Low;
  UINT32 High;
  UINT32 Middle;
  UINT8 Month;

  Days = (Seconds / RTC_SECONDS_PER_DAY) + RTC_DAYS_BEFORE_1900;
  Low = 1900U;
  High = 10000U;
  while ((High - Low) > 1U)
  {
    Middle = Low + ((High - Low) / 2U);
    if (DaysBeforeYear (Middle) <= Days)
    {
      Low = Middle;
    }
    else
    {
      High = Middle;
    }
  }

  Days -= DaysBeforeYear (Low);
  Month = 1U;
  while (Days >= MonthDays (Low, Month))
  {
    Days -= MonthDays (Low, Month);
    ++Month;
  }

  Seconds %= RTC_SECONDS_PER_DAY;
  Time->Year = (UINT16)Low;
  Time->Month = Month;
  Time->Day = (UINT8)(Days + 1ULL);
  Time->Hour = (UINT8)(Seconds / 3600ULL);
  Time->Minute = (UINT8)((Seconds / 60ULL) % 60ULL);
  Time->Second = (UINT8)(Seconds % 60ULL);
  Time->Pad1 = 0U;
  Time->Pad2 = 0U;
}

// floor(Remainder * 1e9 / Frequency), without an overflowing 64-bit product or
// compiler-provided 128-bit division helper. Both additions reduce modulo the
// divisor before they can overflow, even for Frequency == MAX_UINT64.
STATIC
UINT32
FractionNanoseconds (UINT64 Remainder, UINT64 Frequency)
{
  UINT64 Rest;
  UINT32 Result;
  UINT32 Bit;

  Rest = 0ULL;
  Result = 0U;
  for (Bit = 1U << 29; Bit != 0U; Bit >>= 1)
  {
    Result <<= 1;
    if (Rest >= (Frequency - Rest))
    {
      Rest -= Frequency - Rest;
      ++Result;
    }
    else
    {
      Rest += Rest;
    }

    if ((RTC_NANOSECONDS & Bit) != 0U)
    {
      if (Rest >= (Frequency - Remainder))
      {
        Rest -= Frequency - Remainder;
        ++Result;
      }
      else
      {
        Rest += Remainder;
      }
    }
  }

  return Result;
}

STATIC
BOOLEAN
CounterInRange (UINT64 Counter)
{
  if (mRtc.CounterStart < mRtc.CounterEnd)
  {
    return (BOOLEAN)((Counter >= mRtc.CounterStart) && (Counter <= mRtc.CounterEnd));
  }

  return (BOOLEAN)((Counter <= mRtc.CounterStart) && (Counter >= mRtc.CounterEnd));
}

STATIC
UINT64
ElapsedTicks (UINT64 Counter)
{
  // At most one rollover since initialization/SetTime can be distinguished.
  // ArmArchTimerLib advertises an increasing 64-bit counter. No elapsed-time
  // estimate here establishes either a hardware RTC or calendar accuracy.
  if (mRtc.CounterStart < mRtc.CounterEnd)
  {
    if (Counter >= mRtc.Anchor)
    {
      return Counter - mRtc.Anchor;
    }

    return (mRtc.CounterEnd - mRtc.Anchor) + (Counter - mRtc.CounterStart) + 1ULL;
  }

  if (Counter <= mRtc.Anchor)
  {
    return mRtc.Anchor - Counter;
  }

  return (mRtc.Anchor - mRtc.CounterEnd) + (mRtc.CounterStart - Counter) + 1ULL;
}

EFI_STATUS
EFIAPI
LibGetTime (OUT EFI_TIME *Time, OUT EFI_TIME_CAPABILITIES *Capabilities OPTIONAL)
{
  UINT64 Counter;
  UINT64 Ticks;
  UINT64 Seconds;
  UINT32 Nanosecond;
  INT64 LocalSeconds;

  if (Time == NULL)
  {
    return EFI_INVALID_PARAMETER;
  }

  if (!mRtc.Ready)
  {
    return EFI_DEVICE_ERROR;
  }

  Counter = GetPerformanceCounter ();
  if (!CounterInRange (Counter))
  {
    return EFI_DEVICE_ERROR;
  }

  Ticks = ElapsedTicks (Counter);
  Seconds = Ticks / mRtc.Frequency;
  Nanosecond = mRtc.Nanosecond + FractionNanoseconds (Ticks % mRtc.Frequency, mRtc.Frequency);
  LocalSeconds = mRtc.LocalSeconds;
  if (Seconds > (UINT64)(RTC_LAST_SECOND - LocalSeconds))
  {
    return EFI_DEVICE_ERROR;
  }

  LocalSeconds += (INT64)Seconds;
  if (Nanosecond >= RTC_NANOSECONDS)
  {
    if (LocalSeconds == RTC_LAST_SECOND)
    {
      return EFI_DEVICE_ERROR;
    }

    ++LocalSeconds;
    Nanosecond -= RTC_NANOSECONDS;
  }

  SecondsToCalendar ((UINT64)LocalSeconds, Time);
  Time->Nanosecond = Nanosecond;
  Time->TimeZone = mRtc.TimeZone;
  Time->Daylight = mRtc.Daylight;
  if (Capabilities != NULL)
  {
    Capabilities->Resolution =
        (UINT32)((mRtc.Frequency < RTC_NANOSECONDS) ? mRtc.Frequency : RTC_NANOSECONDS);
    Capabilities->Accuracy = 0U; // Counter resolution is known, UTC accuracy is not.
    Capabilities->SetsToZero = FALSE;
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LibSetTime (IN EFI_TIME *Time)
{
  UINT64 Counter;

  if (!ValidTime (Time))
  {
    return EFI_INVALID_PARAMETER;
  }

  if (!mRtc.Ready)
  {
    return EFI_DEVICE_ERROR;
  }

  Counter = GetPerformanceCounter ();
  if (!CounterInRange (Counter))
  {
    return EFI_DEVICE_ERROR;
  }

  // EFI SetTime supplies local calendar time. Zone/daylight are metadata;
  // changing them must not apply a second timezone or seasonal clock shift.
  mRtc.LocalSeconds = CalendarSeconds (Time);
  mRtc.Nanosecond = Time->Nanosecond;
  mRtc.TimeZone = Time->TimeZone;
  mRtc.Daylight = Time->Daylight;
  mRtc.Anchor = Counter;
  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
LibGetWakeupTime (OUT BOOLEAN *Enabled, OUT BOOLEAN *Pending, OUT EFI_TIME *Time)
{
  if ((Enabled == NULL) || (Pending == NULL) || (Time == NULL))
  {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
LibSetWakeupTime (IN BOOLEAN Enabled, OUT EFI_TIME *Time)
{
  if (Enabled && !ValidTime (Time))
  {
    return EFI_INVALID_PARAMETER;
  }

  return EFI_UNSUPPORTED;
}

EFI_STATUS
EFIAPI
LibRtcInitialize (IN EFI_HANDLE ImageHandle, IN EFI_SYSTEM_TABLE *SystemTable)
{
  INT64 Initial;

  (VOID) ImageHandle;
  (VOID) SystemTable;
  mRtc.Ready = FALSE;
  Initial = (INT64)BUILD_EPOCH;
  if ((Initial < -RTC_1970_FROM_1900) || (Initial > (RTC_LAST_SECOND - RTC_1970_FROM_1900)))
  {
    return EFI_DEVICE_ERROR;
  }

  mRtc.Frequency = GetPerformanceCounterProperties (&mRtc.CounterStart, &mRtc.CounterEnd);
  if ((mRtc.Frequency == 0ULL) || (mRtc.CounterStart == mRtc.CounterEnd))
  {
    return EFI_DEVICE_ERROR;
  }

  mRtc.Anchor = GetPerformanceCounter ();
  if (!CounterInRange (mRtc.Anchor))
  {
    return EFI_DEVICE_ERROR;
  }

  mRtc.LocalSeconds = Initial + RTC_1970_FROM_1900;
  mRtc.Nanosecond = 0U;
  mRtc.TimeZone = EFI_UNSPECIFIED_TIMEZONE;
  mRtc.Daylight = 0U;
  mRtc.Ready = TRUE;
  return EFI_SUCCESS;
}
