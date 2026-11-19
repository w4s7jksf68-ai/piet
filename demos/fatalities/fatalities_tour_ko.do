* PIET A.1.1 | 맥주세와 교통사고 사망률 | 가이드북 실습
* 새 세션 또는 저장을 마친 세션에서 실행하세요. 그림 두 장은 작업 폴더에 덮어씁니다.
* 이 파일의 주석과 변수 설명은 한국어입니다. 숫자를 재현하려면 위에서부터 실행하세요.

clear all
set more off
set type double
set extcompat on

* 1. 자료를 열고 사망률과 패널 ID 만들기

sysuse fatalities, clear
label variable state "미국 주 약어"
label variable year "관측 연도"
label variable spirits "증류주 소비량"
label variable unemp "주 실업률 (%)"
label variable income "1인당 개인소득 (1987년 달러)"
label variable emppop "주 인구 대비 취업자 비율 (%)"
label variable beertax "맥주 한 상자당 실질 주세 (달러)"
label variable baptist "남침례교 신자 비율 (%)"
label variable mormon "모르몬교 신자 비율 (%)"
label variable drinkage "법정 최저 음주 연령 (세)"
label variable dry "금주 카운티에 거주하는 인구 비율 (%)"
label variable youngdrivers "운전자 중 15~24세 비율 (0~1)"
label variable miles "운전자 1인당 평균 주행거리 (마일)"
label variable breath "예비 음주 호흡측정법 시행 여부 (yes/no)"
label variable jail "음주운전 초범 의무 구금 여부 (yes/no)"
label variable service "음주운전 초범 의무 사회봉사 여부 (yes/no)"
label variable fatal "교통사고 사망자 수 (명)"
label variable nfatal "야간 교통사고 사망자 수 (명)"
label variable sfatal "단독차량 사고 사망자 수 (명)"
label variable fatal1517 "15~17세 교통사고 사망자 수 (명)"
label variable nfatal1517 "15~17세 야간 교통사고 사망자 수 (명)"
label variable fatal1820 "18~20세 교통사고 사망자 수 (명)"
label variable nfatal1820 "18~20세 야간 교통사고 사망자 수 (명)"
label variable fatal2124 "21~24세 교통사고 사망자 수 (명)"
label variable nfatal2124 "21~24세 야간 교통사고 사망자 수 (명)"
label variable afatal "음주 관련 교통사고 사망자 수 (자료 추계, 명)"
label variable pop "주 전체 인구 (명)"
label variable pop1517 "15~17세 인구 (명)"
label variable pop1820 "18~20세 인구 (명)"
label variable pop2124 "21~24세 인구 (명)"
label variable milestot "주 전체 연간 차량 주행거리 (백만 마일)"
label variable unempus "미국 전체 실업률 (%)"
label variable emppopus "미국 전체 인구 대비 취업자 비율 (%)"
label variable gsp "주내총생산 변화율 (0.01은 1%)"
describe
gen frate   = fatal/pop*10000
label variable frate "인구 1만 명당 교통사고 사망자 수 (명)"
gen lincome = log(income)
label variable lincome "1인당 실질 개인소득의 자연로그"
gen lfatal  = log(fatal)
label variable lfatal "교통사고 사망자 수의 자연로그"
gen lpop    = log(pop)
label variable lpop "주 전체 인구의 자연로그"
gen drinkd  = (breath == "yes")
label variable drinkd "해당 연도 호흡측정법 시행 (1 시행, 0 미시행)"
egen statenum = group(state)
label variable statenum "패널 분석용 주 식별번호"
xtset statenum year
summarize frate beertax unemp income
tabulate breath

* 2. 전체 주와 연도를 모아 맥주세와 사망률 비교하기

regress frate beertax
estat hettest
predict ols_resid, residuals
label variable ols_resid "단순 맥주세 회귀 잔차 (실제율 - 예측률)"
regress frate beertax, vce(robust)
regress frate beertax unemp lincome
estat vif
estat ovtest

* 3. 같은 주 안에서 세율이 변한 해 비교하기

xtreg frate beertax unemp lincome, fe vce(cluster statenum)
reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)
test beertax unemp lincome
xtreg frate beertax unemp lincome, re

* 4. 같은 계수의 불확실성 비교하기

reghdfe frate beertax unemp lincome, absorb(statenum year)
reghdfe frate beertax unemp lincome, absorb(statenum year) vce(robust)
reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)

* 같은 모형을 주 고정효과와 연도 더미로 적어 와일드 군집 부트스트랩을 실행합니다.
reghdfe frate beertax unemp lincome i.year, absorb(statenum) vce(cluster statenum)
boottest beertax, reps(9999) seed(42)

* 5. 도구변수의 관련성과 배제제약 구별하기

regress beertax baptist unemp lincome
test baptist
regress beertax dry unemp lincome
test dry
regress frate baptist unemp lincome

* 6. 사망자 수와 인구 규모를 로그로 살펴보기

reghdfe lfatal beertax unemp lincome lpop, absorb(statenum year) vce(cluster statenum)

* 7. 호흡측정법 시행 시점으로 집단을 나눠 효과 추정하기

gen treatyr = year if drinkd == 1
label variable treatyr "호흡측정법 시행 중인 관측의 연도"
egen gvar = min(treatyr), by(statenum)
label variable gvar "최초 관측 시행 연도 (0은 기간 내 미시행)"
replace gvar = 0 if missing(gvar)
gen byte evertreat = (gvar > 0)
label variable evertreat "관측 기간 중 한 번이라도 법 시행 (1 예, 0 아니오)"
tabulate gvar
csdid frate unemp lincome, ivar(statenum) time(year) gvar(gvar) method(dripw)
estat simple
estat event
estat calendar
estat group

* 8. 산점도, 계수 그림, 집단별 평균 추세 그리기

twoway (scatter frate beertax) (lfit frate beertax), xtitle("맥주 한 상자당 실질세 (달러)") ytitle("인구 1만 명당 사망자 수")
graph export "tour_ko_scatter.svg", replace
eststo clear
quietly reghdfe frate beertax unemp lincome, absorb(statenum year) vce(cluster statenum)
eststo m_fe
label variable _est_m_fe "저장한 이원 고정효과 모형의 추정 표본 (1 포함)"
coefplot m_fe, keep(beertax)
preserve
collapse (mean) frate, by(year evertreat)
twoway (line frate year if evertreat==1) (line frate year if evertreat==0), xtitle("연도") ytitle("인구 1만 명당 평균 사망자 수")
graph export "tour_ko_did.svg", replace
restore

* 9. 전년 값과 변화량으로 동학 살펴보기

regress frate L.beertax unemp lincome
regress frate beertax L.beertax unemp lincome
regress frate D.beertax unemp lincome
regress frate L.frate beertax

* 336행, 45열인지, 한글 레이블이 보이는지 확인합니다.
describe
* 레이블을 보존하려면 아래 줄의 맨 앞 별표를 지우고 새 파일 이름으로 실행하세요.
* save "fatalities_study.dta"
