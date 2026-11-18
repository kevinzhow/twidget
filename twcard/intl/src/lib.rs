use fixed_decimal::{Decimal, FloatPrecision, SignedRoundingMode, UnsignedRoundingMode};
use icu::calendar::Calendar;
use icu::casemap::CaseMapper;
use icu::collator::{Collator, options::CollatorOptions, options::Strength};
use icu::datetime::{
    DateTimeFormatter, DateTimeFormatterPreferences,
    fieldsets::{self, builder::{DateFields, FieldSetBuilder}},
    input::{Date, DateTime, Time},
    options::{Alignment, Length, SubsecondDigits, TimePrecision, YearStyle},
    preferences::HourCycle,
};
use icu::decimal::{DecimalFormatter, DecimalFormatterPreferences, options::GroupingStrategy, provider::{Baked, DecimalDigitsV1}};
use icu::experimental::compactdecimal::{CompactDecimalFormatter, CompactDecimalFormatterPreferences};
use icu::experimental::dimension::{
    currency::{CurrencyCode, formatter::CurrencyFormatter, options::Width},
    percent::formatter::PercentFormatter,
};
use icu::experimental::relativetime::{
    RelativeTimeFormatter, RelativeTimeFormatterOptions, options::Numeric,
};
use icu::list::{ListFormatter, options::{ListFormatterOptions, ListLength}};
use icu::locale::{Locale, LocaleCanonicalizer, LocaleExpander, extensions::unicode::Value};
use icu::plurals::{PluralCategory, PluralRules, PluralRuleType, PluralRulesOptions};
use icu_provider::prelude::*;
use std::{any::TypeId, cell::RefCell, cmp::Ordering, ffi::c_void, fmt, slice, str};
use writeable::{Part, PartsWrite, Writeable};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Text {
    data: *const u8,
    size: usize,
}

impl Text {
    // The caller owns all inputs for the duration of twcard_icu4x_format.
    unsafe fn read<'a>(self) -> Option<&'a str> {
        if self.size == 0 {
            return Some("");
        }
        if self.data.is_null() {
            return None;
        }
        str::from_utf8(unsafe { slice::from_raw_parts(self.data, self.size) }).ok()
    }
}

#[repr(C)]
pub struct OptionEntry {
    name: Text,
    value: Text,
}

#[repr(C)]
pub struct Request {
    kind: Text,
    locale: Text,
    args: *const Text,
    arg_count: usize,
    options: *const OptionEntry,
    option_count: usize,
    number: f64,
    local: [i32; 7],
}

type Emit = extern "C" fn(*mut c_void, Text, Text);

#[derive(Default)]
struct Output {
    parts: Vec<(&'static str, String)>,
    current: &'static str,
}

fn ecma_part(part: Part) -> &'static str {
    match part.value {
        "integer" => "integer",
        "group" => "group",
        "decimal" => "decimal",
        "fraction" => "fraction",
        "minusSign" | "minus_sign" => "minusSign",
        "plusSign" | "plus_sign" => "plusSign",
        "percentSign" | "percent_sign" => "percentSign",
        "currency" => "currency",
        "year" => "year",
        "month" => "month",
        "day" => "day",
        "weekday" => "weekday",
        "hour" => "hour",
        "minute" => "minute",
        "second" => "second",
        "fractionalSecond" | "fractional_second" => "fractionalSecond",
        "dayPeriod" | "day_period" => "dayPeriod",
        "era" => "era",
        _ => "literal",
    }
}

impl fmt::Write for Output {
    fn write_str(&mut self, text: &str) -> fmt::Result {
        if !text.is_empty() {
            let name = if self.current.is_empty() { "literal" } else { self.current };
            if let Some(last) = self.parts.last_mut() {
                if last.0 == name {
                    last.1.push_str(text);
                    return Ok(());
                }
            }
            self.parts.push((name, text.to_owned()));
        }
        Ok(())
    }
}

impl PartsWrite for Output {
    type SubPartsWrite = Self;
    fn with_part(&mut self, part: Part, mut write: impl FnMut(&mut Self) -> fmt::Result) -> fmt::Result {
        let previous = self.current;
        self.current = ecma_part(part);
        let result = write(self);
        self.current = previous;
        result
    }
}

impl Output {
    fn write(value: impl Writeable) -> Option<Self> {
        let mut result = Self::default();
        value.write_to_parts(&mut result).ok()?;
        Some(result)
    }

    fn literal(value: impl Into<String>) -> Option<Self> {
        Some(Self { parts: vec![("literal", value.into())], current: "" })
    }
}

fn decimal(
    number: f64, text: Option<&str>, scale: i16, min: i16, max: i16, integer: i16,
) -> Option<Decimal> {
    let mut result = if let Some(text) = text {
        text.parse::<Decimal>().ok()?
    } else {
        Decimal::try_from_f64(number, FloatPrecision::RoundTrip).ok()?
    };
    result.multiply_pow10(scale);
    result.round_with_mode(-max, SignedRoundingMode::Unsigned(UnsignedRoundingMode::HalfExpand));
    result.trim_end();
    result.pad_end(-min);
    result.pad_start(integer);
    Some(result)
}

#[derive(Default)]
struct NumberingSystemProvider(RefCell<String>);

// The ICU4X decimal provider's successful digits request identifies the selected numbering system.
impl<M: DataMarker> DataProvider<M> for NumberingSystemProvider
where
    Baked: DataProvider<M>,
{
    fn load(&self, request: DataRequest) -> Result<DataResponse<M>, DataError> {
        let response = Baked.load(request)?;
        if TypeId::of::<M>() == TypeId::of::<DecimalDigitsV1>() {
            request.id.marker_attributes.as_str().clone_into(&mut self.0.borrow_mut());
        }
        Ok(response)
    }
}

fn resolve_options(
    kind: &str,
    locale: &Locale,
    decimal_prefs: DecimalFormatterPreferences,
    date_prefs: DateTimeFormatterPreferences,
) -> Option<Output> {
    let provider = NumberingSystemProvider::default();
    DecimalFormatter::try_new_unstable(&provider, decimal_prefs, Default::default()).ok()?;
    let mut output = Output::default();
    output.parts.push(("locale", locale.to_string()));
    output.parts.push(("numberingSystem", provider.0.into_inner()));
    if kind == "dateOptions" {
        let format = DateTimeFormatter::try_new(date_prefs, fieldsets::YMD::short()).ok()?;
        let calendar = format.calendar().0.calendar_algorithm()?;
        output.parts.push(("calendar", Value::from(calendar).to_string()));
    }
    Some(output)
}

fn format_date<'options>(
    prefs: DateTimeFormatterPreferences,
    option: impl Fn(&str) -> Option<&'options str>,
    local: &[i32; 7],
) -> Option<Output> {
    let date = Date::try_new_iso(local[0], local[1].try_into().ok()?, local[2].try_into().ok()?).ok()?;
    let time = Time::try_new(
        local[3].try_into().ok()?, local[4].try_into().ok()?, local[5].try_into().ok()?,
        local[6].try_into().ok()?,
    ).ok()?;
    let datetime = DateTime { date, time };
    let date_style = option("dateStyle");
    let time_style = option("timeStyle");
    let has_hour = option("hour").is_some();
    let has_minute = option("minute").is_some();
    let has_second = option("second").is_some();
    let fraction = option("fractionalSecondDigits");
    let has_time = time_style.is_some() || has_hour || has_minute || has_second || fraction.is_some();
    let year = option("year");
    let month = option("month");
    let day = option("day");
    let weekday = option("weekday");
    let era = option("era");
    let has_fields = year.is_some() || month.is_some() || day.is_some() || weekday.is_some() || era.is_some();

    let mut fields = FieldSetBuilder::new();
    if let Some(style) = date_style {
        fields.date_fields = Some(if style == "full" { DateFields::YMDE } else { DateFields::YMD });
    } else if has_fields {
        fields.date_fields = Some(match (year.is_some(), month.is_some(), day.is_some(), weekday.is_some()) {
            (true, true, true, true) => DateFields::YMDE,
            (true, true, true, false) => DateFields::YMD,
            (false, true, true, true) => DateFields::MDE,
            (false, true, true, false) => DateFields::MD,
            (false, false, true, true) => DateFields::DE,
            (false, false, true, false) => DateFields::D,
            (false, false, false, true) => DateFields::E,
            (true, true, false, false) => DateFields::YM,
            (true, false, false, false) => DateFields::Y,
            (false, true, false, false) => DateFields::M,
            _ => return None,
        });
    } else if !has_time {
        fields.date_fields = Some(DateFields::YMD);
    }
    fields.length = Some(match date_style.or(month).or(weekday).or(time_style) {
        Some("full" | "long") => Length::Long,
        Some("medium" | "short") if date_style != Some("short") => Length::Medium,
        _ => Length::Short,
    });
    if era.is_some() {
        fields.year_style = Some(YearStyle::WithEra);
    } else if (year.is_some() || date_style.is_some() || !has_fields) &&
        fields.date_fields.is_some() && date_style != Some("short") && year != Some("2-digit") {
        fields.year_style = Some(YearStyle::Full);
    }
    if [month, day, option("hour"), option("minute"), option("second")].contains(&Some("2-digit")) {
        fields.alignment = Some(Alignment::Column);
    }
    if has_time {
        fields.time_precision = Some(match fraction {
            Some("1") => TimePrecision::Subsecond(SubsecondDigits::S1),
            Some("2") => TimePrecision::Subsecond(SubsecondDigits::S2),
            Some("3") => TimePrecision::Subsecond(SubsecondDigits::S3),
            _ if has_second || time_style.is_some_and(|s| s != "short") => TimePrecision::Second,
            _ if has_minute || time_style == Some("short") => TimePrecision::Minute,
            _ => TimePrecision::Hour,
        });
        if fields.date_fields.is_none() {
            let fmt = DateTimeFormatter::try_new(prefs, fields.build_time().ok()?).ok()?;
            return Output::write(fmt.format(&datetime.time));
        }
        let fmt = DateTimeFormatter::try_new(prefs, fields.build_date_and_time().ok()?).ok()?;
        return Output::write(fmt.format(&datetime));
    }
    if matches!(fields.date_fields, Some(DateFields::Y | DateFields::YM | DateFields::M)) {
        let fmt = DateTimeFormatter::try_new(prefs, fields.build_calendar_period().ok()?).ok()?;
        return Output::write(fmt.format(&datetime.date));
    }
    let fmt = DateTimeFormatter::try_new(prefs, fields.build_date().ok()?).ok()?;
    Output::write(fmt.format(&datetime.date))
}

macro_rules! relative_units {
    ($which:expr, $locale:expr, $options:expr, $($unit:literal => $constructor:ident),* $(,)?) => {
        match $which {
            $($unit => RelativeTimeFormatter::$constructor($locale.into(), $options).ok(),)*
            _ => None,
        }
    };
}

fn format<'options>(
    kind: &str,
    locale: &Locale,
    args: &[&str],
    option: impl Fn(&str) -> Option<&'options str>,
    number: f64,
    local: &[i32; 7],
) -> Option<Output> {
    let style = option("style").unwrap_or("long");
    let mut decimal_prefs: DecimalFormatterPreferences = locale.into();
    if let Some(numbering) = option("numberingSystem").and_then(|s| s.parse::<Value>().ok()) {
        decimal_prefs.numbering_system = numbering.try_into().ok();
    }
    let mut date_prefs: DateTimeFormatterPreferences = locale.into();
    date_prefs.numbering_system = decimal_prefs.numbering_system;
    if let Some(calendar) = option("calendar").and_then(|s| s.parse::<Value>().ok()) {
        date_prefs.calendar_algorithm = (&calendar).try_into().ok();
    }
    date_prefs.hour_cycle = match option("hour12") {
        Some("true") => Some(HourCycle::H12),
        Some("false") => Some(HourCycle::H23),
        _ => option("hourCycle").and_then(|s| s.parse::<Value>().ok())
            .and_then(|value| (&value).try_into().ok()).or(date_prefs.hour_cycle),
    };
    match kind {
        "numberOptions" | "dateOptions" => resolve_options(kind, locale, decimal_prefs, date_prefs),
        "number" | "numberParts" => {
            let percent = style == "percent";
            let currency = style == "currency";
            if style == "unit" || (currency && kind == "numberParts") {
                return None;
            }
            let mut opts = icu::decimal::options::DecimalFormatterOptions::default();
            opts.grouping_strategy = Some(match option("useGrouping") {
                Some("false") => GroupingStrategy::Never,
                Some("min2") => GroupingStrategy::Min2,
                Some("always") => GroupingStrategy::Always,
                _ => GroupingStrategy::Auto,
            });
            if !percent && !currency && option("notation") == Some("compact") && kind == "number" {
                let mut prefs: CompactDecimalFormatterPreferences = locale.into();
                prefs.numbering_system = decimal_prefs.numbering_system;
                let compact = if option("compactDisplay") == Some("long") {
                    CompactDecimalFormatter::try_new_long(prefs, opts.into())
                } else {
                    CompactDecimalFormatter::try_new_short(prefs, opts.into())
                }.ok()?;
                let value = decimal(number, args.first().copied(), 0, 0, 100, 1)?;
                return Output::write(compact.format_fixed_decimal(&value));
            }
            let formatter = DecimalFormatter::try_new(decimal_prefs, opts).ok()?;
            let min = option("minimumFractionDigits").and_then(|n| n.parse().ok())
                .unwrap_or(if currency { 2i16 } else { 0i16 }).clamp(0, 100);
            let max = option("maximumFractionDigits").and_then(|n| n.parse().ok())
                .unwrap_or(if percent { 0 } else if currency { 2 } else { 3 }).clamp(min, 100);
            let integer = option("minimumIntegerDigits").and_then(|n| n.parse().ok()).unwrap_or(1i16).clamp(1, 21);
            if option("notation") == Some("compact") {
                return None;
            }
            let value = decimal(number, args.first().copied(), if percent { 2 } else { 0 }, min, max, integer)?;
            if currency {
                let code = CurrencyCode(option("currency")?.parse().ok()?);
                let width = match option("currencyDisplay") {
                    Some("symbol") | None => Width::Short,
                    Some("narrowSymbol") => Width::Narrow,
                    _ => return None,
                };
                if option("currencySign") == Some("accounting") {
                    return None;
                }
                let fmt = CurrencyFormatter::try_new(locale.into(), width.into()).ok()?;
                return Output::literal(fmt.format_fixed_decimal(&value, code).write_to_string());
            }
            if percent && kind == "number" {
                let fmt = PercentFormatter::try_new_with_decimal_formatter(
                    locale.into(), formatter, Default::default()).ok()?;
                return Output::literal(fmt.format(&value).write_to_string());
            }
            let mut result = Output::write(formatter.format(&value))?;
            if percent {
                result.parts.push(("percentSign", "%".into()));
            }
            Some(result)
        }
        "date" | "dateParts" => format_date(date_prefs, option, local),
        "plural" => {
            let mut options = PluralRulesOptions::default();
            options.rule_type = Some(if option("type") == Some("ordinal") {
                PluralRuleType::Ordinal
            } else {
                PluralRuleType::Cardinal
            });
            let rules = PluralRules::try_new(locale.into(), options).ok()?;
            let value = decimal(number, args.first().copied(), 0, 0, 3, 1)?;
            Output::literal(match rules.category_for(&value) {
                PluralCategory::Zero => "zero",
                PluralCategory::One => "one",
                PluralCategory::Two => "two",
                PluralCategory::Few => "few",
                PluralCategory::Many => "many",
                PluralCategory::Other => "other",
            })
        }
        "list" => {
            let opts = ListFormatterOptions::default().with_length(match style {
                "short" => ListLength::Short,
                "narrow" => ListLength::Narrow,
                _ => ListLength::Wide,
            });
            let fmt = match option("type") {
                Some("disjunction") => ListFormatter::try_new_or(locale.into(), opts),
                Some("unit") => ListFormatter::try_new_unit(locale.into(), opts),
                _ => ListFormatter::try_new_and(locale.into(), opts),
            }.ok()?;
            Output::write(fmt.format(args.iter()))
        }
        "compare" => {
            let mut opts = CollatorOptions::default();
            opts.strength = Some(match option("sensitivity") {
                Some("base") => Strength::Primary,
                Some("accent") => Strength::Secondary,
                _ => Strength::Tertiary,
            });
            let collator = Collator::try_new(locale.into(), opts).ok()?;
            Output::literal(match collator.compare(args.first()?, args.get(1)?) {
                Ordering::Less => "-1",
                Ordering::Equal => "0",
                Ordering::Greater => "1",
            })
        }
        "upper" => Output::literal(CaseMapper::new().uppercase_to_string(args.first()?, &locale.id)),
        "lower" => Output::literal(CaseMapper::new().lowercase_to_string(args.first()?, &locale.id)),
        "relative" => {
            let unit = option("unit")?.trim_end_matches('s');
            let options = RelativeTimeFormatterOptions {
                numeric: if option("numeric") == Some("auto") { Numeric::Auto } else { Numeric::Always },
            };
            let formatter = match style {
                "narrow" => relative_units!(unit, locale, options,
                    "second" => try_new_narrow_second, "minute" => try_new_narrow_minute, "hour" => try_new_narrow_hour,
                    "day" => try_new_narrow_day, "week" => try_new_narrow_week, "month" => try_new_narrow_month,
                    "quarter" => try_new_narrow_quarter, "year" => try_new_narrow_year),
                "short" => relative_units!(unit, locale, options,
                    "second" => try_new_short_second, "minute" => try_new_short_minute, "hour" => try_new_short_hour,
                    "day" => try_new_short_day, "week" => try_new_short_week, "month" => try_new_short_month,
                    "quarter" => try_new_short_quarter, "year" => try_new_short_year),
                _ => relative_units!(unit, locale, options,
                    "second" => try_new_long_second, "minute" => try_new_long_minute, "hour" => try_new_long_hour,
                    "day" => try_new_long_day, "week" => try_new_long_week, "month" => try_new_long_month,
                    "quarter" => try_new_long_quarter, "year" => try_new_long_year),
            }?;
            Output::write(formatter.format(decimal(number, None, 0, 0, 3, 1)?))
        }
        _ => None,
    }
}

fn ffi_text(value: &str) -> Text {
    Text { data: value.as_ptr(), size: value.len() }
}

/// # Safety
/// The caller keeps the request and its arrays live during this call. The callback
/// copies its text arguments before returning, and never unwinds into Rust.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn twcard_icu4x_format(request: &Request, emit: Emit, opaque: *mut c_void) -> bool {
    let output = (|| {
        let kind = unsafe { request.kind.read() }?;
        let mut locale: Locale = unsafe { request.locale.read() }?.parse().ok()?;
        LocaleCanonicalizer::new_extended().canonicalize(&mut locale);
        let mut language = locale.id.language.as_str().parse().ok()?;
        LocaleExpander::new_extended().maximize(&mut language);
        if language.script.is_none() || locale.id.language.is_unknown() {
            return None;
        }
        let texts = if request.arg_count == 0 { &[] } else {
            unsafe { slice::from_raw_parts(request.args, request.arg_count) }
        };
        let values = texts.iter().map(|value| unsafe { value.read() }).collect::<Option<Vec<_>>>()?;
        let options = if request.option_count == 0 { &[] } else {
            unsafe { slice::from_raw_parts(request.options, request.option_count) }
        };
        let option = |query: &str| options.iter().find_map(|entry| {
            if unsafe { entry.name.read() }? == query { unsafe { entry.value.read() } } else { None }
        });
        format(kind, &locale, &values, option, request.number, &request.local)
    })();
    let Some(result) = output else { return false; };
    for (kind, text) in result.parts {
        emit(opaque, ffi_text(kind), ffi_text(&text));
    }
    true
}
