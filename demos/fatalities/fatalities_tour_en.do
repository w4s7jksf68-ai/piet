* PIET A.1.1 | Beer taxes and traffic fatalities | Hands-on lab
* Start in a fresh session or save your work first. The two SVG exports overwrite files of the same name.
* Run the numbered sections in order to reproduce the results in the lab guide.

clear all
set more off
set type double
set extcompat on

* 1. Load the panel, construct a fatality rate and identify states

sysuse fatalities, clear
label variable state "US state abbreviation"
label variable year "Calendar year"
label variable spirits "Spirits consumption"
label variable unemp "State unemployment rate (%)"
label variable income "Personal income per capita (1987 dollars)"
label variable emppop "State employment-to-population ratio (%)"
label variable beertax "Real state tax per case of beer (dollars)"
label variable baptist "Southern Baptists as a share of the population (%)"
label variable mormon "Mormons as a share of the population (%)"
label variable drinkage "Minimum legal drinking age (years)"
label variable dry "Population living in dry counties (%)"
label variable youngdrivers "Share of drivers aged 15-24 (fraction from 0 to 1)"
label variable miles "Average miles driven per driver"
label variable breath "Preliminary breath-test law in force (yes/no)"
label variable jail "Mandatory jail for a first drunk-driving conviction (yes/no)"
label variable service "Mandatory community service for a first drunk-driving conviction (yes/no)"
label variable fatal "Traffic fatalities (people)"
label variable nfatal "Nighttime traffic fatalities (people)"
label variable sfatal "Single-vehicle traffic fatalities (people)"
label variable fatal1517 "Traffic fatalities, ages 15-17 (people)"
label variable nfatal1517 "Nighttime traffic fatalities, ages 15-17 (people)"
label variable fatal1820 "Traffic fatalities, ages 18-20 (people)"
label variable nfatal1820 "Nighttime traffic fatalities, ages 18-20 (people)"
label variable fatal2124 "Traffic fatalities, ages 21-24 (people)"
label variable nfatal2124 "Nighttime traffic fatalities, ages 21-24 (people)"
label variable afatal "Estimated alcohol-involved traffic fatalities (people)"
label variable pop "State population (people)"
label variable pop1517 "Population ages 15-17 (people)"
label variable pop1820 "Population ages 18-20 (people)"
label variable pop2124 "Population ages 21-24 (people)"
label variable milestot "Total vehicle miles traveled in the state (millions)"
label variable unempus "US unemployment rate (%)"
label variable emppopus "US employment-to-population ratio (%)"
label variable gsp "Growth rate of gross state product (0.01 means 1%)"
describe
gen frate   = fatal/pop*10000
label variable frate "Traffic deaths per 10,000 residents"
gen lincome = log(income)
label variable lincome "Natural log of real personal income per capita"
gen lfatal  = log(fatal)
label variable lfatal "Natural log of traffic fatalities"
gen lpop    = log(pop)
label variable lpop "Natural log of state population"
gen drinkd  = (breath == "yes")
label variable drinkd "Breath-test law in force this year (1 yes, 0 no)"
egen statenum = group(state)
label variable statenum "Numeric state ID for panel analysis"
xtset statenum year
summarize frate beertax unemp income
tabulate breath

* 2. Compare taxes and fatality rates across all state-years

regress frate beertax
estat hettest
predict ols_resid, residuals
label variable ols_resid "Pooled OLS residual (observed minus fitted fatality rate)"
regress frate beertax, vce(robust)
regress frate beertax unemp lincome
estat vif
estat ovtest

* 3. Compare tax changes within states

xtreg frate beertax unemp lincome, fe vce(cluster statenum)
reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)
test beertax unemp lincome
xtreg frate beertax unemp lincome, re

* 4. Estimate uncertainty around the same slope

reghdfe frate beertax unemp lincome, absorb(statenum year)
reghdfe frate beertax unemp lincome, absorb(statenum year) vce(robust)
reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)

* Enter year indicators explicitly so the wild cluster bootstrap can run after absorbing state effects.
reghdfe frate beertax unemp lincome i.year, absorb(statenum) vce(cluster statenum)
boottest beertax, reps(9999) seed(42)

* 5. Check instrument relevance and consider the exclusion restriction

regress beertax baptist unemp lincome
test baptist
regress beertax dry unemp lincome
test dry
regress frate baptist unemp lincome

* 6. Model log death counts with population as a covariate

reghdfe lfatal beertax unemp lincome lpop, absorb(statenum year) vce(cluster statenum)

* 7. Group states by the first observed year of the breath-test law

gen treatyr = year if drinkd == 1
label variable treatyr "Year of observation when the breath-test law is in force"
egen gvar = min(treatyr), by(statenum)
label variable gvar "First observed year with the law (0 if never observed in force)"
replace gvar = 0 if missing(gvar)
gen byte evertreat = (gvar > 0)
label variable evertreat "Law ever observed in force during the panel (1 yes, 0 no)"
tabulate gvar
csdid frate unemp lincome, ivar(statenum) time(year) gvar(gvar) method(dripw)
estat simple
estat event
estat calendar
estat group

* 8. Plot the raw association, the adjusted coefficient and group trends

twoway (scatter frate beertax) (lfit frate beertax), xtitle("Real beer tax per case (dollars)") ytitle("Deaths per 10,000 residents")
graph export "tour_en_scatter.svg", replace
eststo clear
quietly reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)
eststo m_fe
label variable _est_m_fe "Observation used in the stored two-way FE model (1 included)"
coefplot m_fe, keep(beertax)
preserve
collapse (mean) frate, by(year evertreat)
twoway (line frate year if evertreat==1) (line frate year if evertreat==0), xtitle("Year") ytitle("Mean deaths per 10,000 residents")
graph export "tour_en_did.svg", replace
restore

* 9. Compare current values, lags and year-to-year changes

regress frate L.beertax unemp lincome
regress frate beertax L.beertax unemp lincome
regress frate D.beertax unemp lincome
regress frate L.frate beertax

* Check for 336 rows, 45 columns and English variable labels.
describe
* To keep the labels, uncomment the next line and choose a new filename.
* save "fatalities_study.dta"
