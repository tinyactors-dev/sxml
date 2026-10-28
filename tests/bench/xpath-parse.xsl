<?xml version="1.0"?>
<!-- Parsing XPath (packet perf-17): 250 template rules whose assertions
     have the shape of the EN 16931 Schematron's, each text distinct, and
     run over this stylesheet itself, so the time is mostly the compile
     and in it the XPath front end. Only the first rule is applied; it
     writes its number when its test holds. -->
<xsl:stylesheet version="3.0" xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:cac="urn:example:aggregate" xmlns:cbc="urn:example:basic">
  <xsl:template match="/">
    <r><xsl:apply-templates select="(//xsl:template)[1]" mode="check"/></r>
  </xsl:template>
  <xsl:template match="xsl:template[1]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(1) * 2 + sum(//cbc:LineAmount1 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge1[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 1.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">1 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[2]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(2) * 2 + sum(//cbc:LineAmount2 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge2[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 2.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">2 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[3]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(3) * 2 + sum(//cbc:LineAmount3 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge3[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 3.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">3 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[4]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(4) * 2 + sum(//cbc:LineAmount4 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge4[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 4.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">4 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[5]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(5) * 2 + sum(//cbc:LineAmount5 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge5[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 5.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">5 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[6]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(6) * 2 + sum(//cbc:LineAmount6 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge6[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 6.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">6 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[7]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(7) * 2 + sum(//cbc:LineAmount7 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge7[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 7.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">7 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[8]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(8) * 2 + sum(//cbc:LineAmount8 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge8[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 8.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">8 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[9]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(9) * 2 + sum(//cbc:LineAmount9 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge9[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 9.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">9 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[10]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(10) * 2 + sum(//cbc:LineAmount10 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge10[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 10.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">10 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[11]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(11) * 2 + sum(//cbc:LineAmount11 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge11[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 11.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">11 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[12]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(12) * 2 + sum(//cbc:LineAmount12 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge12[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 12.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">12 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[13]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(13) * 2 + sum(//cbc:LineAmount13 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge13[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 13.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">13 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[14]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(14) * 2 + sum(//cbc:LineAmount14 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge14[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 14.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">14 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[15]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(15) * 2 + sum(//cbc:LineAmount15 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge15[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 15.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">15 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[16]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(16) * 2 + sum(//cbc:LineAmount16 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge16[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 16.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">16 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[17]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(17) * 2 + sum(//cbc:LineAmount17 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge17[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 17.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">17 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[18]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(18) * 2 + sum(//cbc:LineAmount18 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge18[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 18.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">18 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[19]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(19) * 2 + sum(//cbc:LineAmount19 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge19[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 19.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">19 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[20]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(20) * 2 + sum(//cbc:LineAmount20 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge20[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 20.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">20 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[21]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(21) * 2 + sum(//cbc:LineAmount21 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge21[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 21.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">21 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[22]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(22) * 2 + sum(//cbc:LineAmount22 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge22[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 22.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">22 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[23]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(23) * 2 + sum(//cbc:LineAmount23 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge23[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 23.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">23 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[24]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(24) * 2 + sum(//cbc:LineAmount24 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge24[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 24.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">24 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[25]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(25) * 2 + sum(//cbc:LineAmount25 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge25[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 25.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">25 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[26]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(26) * 2 + sum(//cbc:LineAmount26 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge26[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 26.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">26 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[27]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(27) * 2 + sum(//cbc:LineAmount27 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge27[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 27.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">27 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[28]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(28) * 2 + sum(//cbc:LineAmount28 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge28[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 28.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">28 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[29]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(29) * 2 + sum(//cbc:LineAmount29 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge29[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 29.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">29 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[30]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(30) * 2 + sum(//cbc:LineAmount30 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge30[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 30.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">30 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[31]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(31) * 2 + sum(//cbc:LineAmount31 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge31[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 31.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">31 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[32]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(32) * 2 + sum(//cbc:LineAmount32 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge32[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 32.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">32 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[33]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(33) * 2 + sum(//cbc:LineAmount33 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge33[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 33.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">33 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[34]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(34) * 2 + sum(//cbc:LineAmount34 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge34[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 34.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">34 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[35]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(35) * 2 + sum(//cbc:LineAmount35 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge35[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 35.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">35 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[36]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(36) * 2 + sum(//cbc:LineAmount36 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge36[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 36.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">36 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[37]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(37) * 2 + sum(//cbc:LineAmount37 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge37[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 37.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">37 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[38]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(38) * 2 + sum(//cbc:LineAmount38 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge38[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 38.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">38 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[39]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(39) * 2 + sum(//cbc:LineAmount39 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge39[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 39.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">39 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[40]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(40) * 2 + sum(//cbc:LineAmount40 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge40[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 40.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">40 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[41]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(41) * 2 + sum(//cbc:LineAmount41 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge41[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 41.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">41 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[42]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(42) * 2 + sum(//cbc:LineAmount42 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge42[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 42.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">42 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[43]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(43) * 2 + sum(//cbc:LineAmount43 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge43[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 43.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">43 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[44]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(44) * 2 + sum(//cbc:LineAmount44 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge44[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 44.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">44 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[45]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(45) * 2 + sum(//cbc:LineAmount45 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge45[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 45.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">45 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[46]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(46) * 2 + sum(//cbc:LineAmount46 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge46[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 46.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">46 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[47]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(47) * 2 + sum(//cbc:LineAmount47 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge47[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 47.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">47 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[48]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(48) * 2 + sum(//cbc:LineAmount48 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge48[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 48.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">48 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[49]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(49) * 2 + sum(//cbc:LineAmount49 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge49[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 49.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">49 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[50]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(50) * 2 + sum(//cbc:LineAmount50 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge50[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 50.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">50 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[51]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(51) * 2 + sum(//cbc:LineAmount51 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge51[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 51.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">51 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[52]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(52) * 2 + sum(//cbc:LineAmount52 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge52[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 52.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">52 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[53]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(53) * 2 + sum(//cbc:LineAmount53 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge53[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 53.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">53 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[54]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(54) * 2 + sum(//cbc:LineAmount54 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge54[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 54.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">54 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[55]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(55) * 2 + sum(//cbc:LineAmount55 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge55[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 55.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">55 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[56]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(56) * 2 + sum(//cbc:LineAmount56 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge56[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 56.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">56 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[57]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(57) * 2 + sum(//cbc:LineAmount57 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge57[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 57.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">57 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[58]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(58) * 2 + sum(//cbc:LineAmount58 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge58[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 58.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">58 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[59]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(59) * 2 + sum(//cbc:LineAmount59 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge59[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 59.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">59 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[60]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(60) * 2 + sum(//cbc:LineAmount60 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge60[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 60.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">60 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[61]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(61) * 2 + sum(//cbc:LineAmount61 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge61[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 61.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">61 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[62]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(62) * 2 + sum(//cbc:LineAmount62 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge62[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 62.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">62 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[63]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(63) * 2 + sum(//cbc:LineAmount63 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge63[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 63.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">63 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[64]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(64) * 2 + sum(//cbc:LineAmount64 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge64[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 64.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">64 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[65]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(65) * 2 + sum(//cbc:LineAmount65 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge65[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 65.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">65 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[66]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(66) * 2 + sum(//cbc:LineAmount66 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge66[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 66.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">66 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[67]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(67) * 2 + sum(//cbc:LineAmount67 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge67[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 67.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">67 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[68]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(68) * 2 + sum(//cbc:LineAmount68 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge68[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 68.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">68 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[69]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(69) * 2 + sum(//cbc:LineAmount69 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge69[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 69.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">69 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[70]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(70) * 2 + sum(//cbc:LineAmount70 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge70[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 70.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">70 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[71]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(71) * 2 + sum(//cbc:LineAmount71 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge71[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 71.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">71 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[72]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(72) * 2 + sum(//cbc:LineAmount72 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge72[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 72.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">72 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[73]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(73) * 2 + sum(//cbc:LineAmount73 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge73[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 73.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">73 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[74]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(74) * 2 + sum(//cbc:LineAmount74 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge74[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 74.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">74 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[75]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(75) * 2 + sum(//cbc:LineAmount75 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge75[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 75.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">75 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[76]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(76) * 2 + sum(//cbc:LineAmount76 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge76[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 76.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">76 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[77]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(77) * 2 + sum(//cbc:LineAmount77 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge77[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 77.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">77 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[78]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(78) * 2 + sum(//cbc:LineAmount78 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge78[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 78.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">78 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[79]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(79) * 2 + sum(//cbc:LineAmount79 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge79[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 79.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">79 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[80]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(80) * 2 + sum(//cbc:LineAmount80 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge80[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 80.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">80 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[81]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(81) * 2 + sum(//cbc:LineAmount81 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge81[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 81.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">81 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[82]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(82) * 2 + sum(//cbc:LineAmount82 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge82[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 82.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">82 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[83]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(83) * 2 + sum(//cbc:LineAmount83 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge83[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 83.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">83 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[84]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(84) * 2 + sum(//cbc:LineAmount84 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge84[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 84.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">84 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[85]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(85) * 2 + sum(//cbc:LineAmount85 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge85[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 85.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">85 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[86]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(86) * 2 + sum(//cbc:LineAmount86 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge86[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 86.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">86 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[87]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(87) * 2 + sum(//cbc:LineAmount87 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge87[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 87.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">87 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[88]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(88) * 2 + sum(//cbc:LineAmount88 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge88[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 88.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">88 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[89]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(89) * 2 + sum(//cbc:LineAmount89 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge89[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 89.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">89 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[90]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(90) * 2 + sum(//cbc:LineAmount90 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge90[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 90.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">90 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[91]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(91) * 2 + sum(//cbc:LineAmount91 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge91[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 91.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">91 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[92]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(92) * 2 + sum(//cbc:LineAmount92 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge92[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 92.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">92 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[93]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(93) * 2 + sum(//cbc:LineAmount93 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge93[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 93.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">93 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[94]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(94) * 2 + sum(//cbc:LineAmount94 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge94[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 94.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">94 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[95]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(95) * 2 + sum(//cbc:LineAmount95 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge95[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 95.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">95 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[96]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(96) * 2 + sum(//cbc:LineAmount96 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge96[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 96.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">96 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[97]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(97) * 2 + sum(//cbc:LineAmount97 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge97[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 97.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">97 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[98]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(98) * 2 + sum(//cbc:LineAmount98 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge98[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 98.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">98 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[99]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(99) * 2 + sum(//cbc:LineAmount99 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge99[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 99.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">99 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[100]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(100) * 2 + sum(//cbc:LineAmount100 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge100[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 100.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">100 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[101]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(101) * 2 + sum(//cbc:LineAmount101 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge101[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 101.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">101 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[102]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(102) * 2 + sum(//cbc:LineAmount102 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge102[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 102.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">102 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[103]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(103) * 2 + sum(//cbc:LineAmount103 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge103[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 103.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">103 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[104]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(104) * 2 + sum(//cbc:LineAmount104 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge104[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 104.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">104 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[105]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(105) * 2 + sum(//cbc:LineAmount105 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge105[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 105.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">105 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[106]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(106) * 2 + sum(//cbc:LineAmount106 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge106[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 106.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">106 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[107]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(107) * 2 + sum(//cbc:LineAmount107 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge107[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 107.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">107 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[108]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(108) * 2 + sum(//cbc:LineAmount108 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge108[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 108.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">108 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[109]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(109) * 2 + sum(//cbc:LineAmount109 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge109[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 109.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">109 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[110]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(110) * 2 + sum(//cbc:LineAmount110 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge110[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 110.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">110 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[111]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(111) * 2 + sum(//cbc:LineAmount111 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge111[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 111.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">111 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[112]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(112) * 2 + sum(//cbc:LineAmount112 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge112[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 112.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">112 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[113]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(113) * 2 + sum(//cbc:LineAmount113 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge113[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 113.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">113 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[114]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(114) * 2 + sum(//cbc:LineAmount114 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge114[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 114.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">114 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[115]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(115) * 2 + sum(//cbc:LineAmount115 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge115[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 115.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">115 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[116]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(116) * 2 + sum(//cbc:LineAmount116 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge116[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 116.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">116 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[117]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(117) * 2 + sum(//cbc:LineAmount117 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge117[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 117.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">117 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[118]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(118) * 2 + sum(//cbc:LineAmount118 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge118[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 118.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">118 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[119]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(119) * 2 + sum(//cbc:LineAmount119 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge119[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 119.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">119 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[120]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(120) * 2 + sum(//cbc:LineAmount120 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge120[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 120.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">120 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[121]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(121) * 2 + sum(//cbc:LineAmount121 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge121[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 121.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">121 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[122]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(122) * 2 + sum(//cbc:LineAmount122 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge122[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 122.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">122 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[123]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(123) * 2 + sum(//cbc:LineAmount123 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge123[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 123.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">123 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[124]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(124) * 2 + sum(//cbc:LineAmount124 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge124[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 124.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">124 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[125]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(125) * 2 + sum(//cbc:LineAmount125 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge125[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 125.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">125 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[126]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(126) * 2 + sum(//cbc:LineAmount126 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge126[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 126.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">126 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[127]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(127) * 2 + sum(//cbc:LineAmount127 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge127[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 127.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">127 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[128]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(128) * 2 + sum(//cbc:LineAmount128 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge128[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 128.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">128 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[129]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(129) * 2 + sum(//cbc:LineAmount129 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge129[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 129.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">129 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[130]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(130) * 2 + sum(//cbc:LineAmount130 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge130[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 130.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">130 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[131]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(131) * 2 + sum(//cbc:LineAmount131 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge131[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 131.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">131 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[132]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(132) * 2 + sum(//cbc:LineAmount132 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge132[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 132.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">132 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[133]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(133) * 2 + sum(//cbc:LineAmount133 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge133[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 133.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">133 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[134]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(134) * 2 + sum(//cbc:LineAmount134 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge134[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 134.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">134 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[135]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(135) * 2 + sum(//cbc:LineAmount135 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge135[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 135.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">135 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[136]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(136) * 2 + sum(//cbc:LineAmount136 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge136[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 136.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">136 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[137]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(137) * 2 + sum(//cbc:LineAmount137 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge137[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 137.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">137 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[138]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(138) * 2 + sum(//cbc:LineAmount138 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge138[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 138.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">138 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[139]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(139) * 2 + sum(//cbc:LineAmount139 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge139[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 139.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">139 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[140]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(140) * 2 + sum(//cbc:LineAmount140 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge140[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 140.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">140 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[141]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(141) * 2 + sum(//cbc:LineAmount141 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge141[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 141.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">141 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[142]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(142) * 2 + sum(//cbc:LineAmount142 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge142[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 142.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">142 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[143]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(143) * 2 + sum(//cbc:LineAmount143 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge143[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 143.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">143 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[144]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(144) * 2 + sum(//cbc:LineAmount144 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge144[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 144.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">144 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[145]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(145) * 2 + sum(//cbc:LineAmount145 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge145[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 145.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">145 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[146]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(146) * 2 + sum(//cbc:LineAmount146 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge146[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 146.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">146 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[147]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(147) * 2 + sum(//cbc:LineAmount147 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge147[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 147.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">147 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[148]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(148) * 2 + sum(//cbc:LineAmount148 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge148[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 148.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">148 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[149]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(149) * 2 + sum(//cbc:LineAmount149 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge149[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 149.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">149 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[150]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(150) * 2 + sum(//cbc:LineAmount150 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge150[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 150.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">150 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[151]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(151) * 2 + sum(//cbc:LineAmount151 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge151[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 151.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">151 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[152]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(152) * 2 + sum(//cbc:LineAmount152 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge152[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 152.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">152 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[153]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(153) * 2 + sum(//cbc:LineAmount153 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge153[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 153.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">153 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[154]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(154) * 2 + sum(//cbc:LineAmount154 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge154[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 154.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">154 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[155]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(155) * 2 + sum(//cbc:LineAmount155 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge155[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 155.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">155 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[156]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(156) * 2 + sum(//cbc:LineAmount156 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge156[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 156.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">156 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[157]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(157) * 2 + sum(//cbc:LineAmount157 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge157[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 157.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">157 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[158]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(158) * 2 + sum(//cbc:LineAmount158 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge158[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 158.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">158 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[159]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(159) * 2 + sum(//cbc:LineAmount159 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge159[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 159.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">159 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[160]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(160) * 2 + sum(//cbc:LineAmount160 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge160[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 160.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">160 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[161]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(161) * 2 + sum(//cbc:LineAmount161 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge161[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 161.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">161 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[162]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(162) * 2 + sum(//cbc:LineAmount162 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge162[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 162.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">162 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[163]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(163) * 2 + sum(//cbc:LineAmount163 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge163[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 163.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">163 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[164]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(164) * 2 + sum(//cbc:LineAmount164 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge164[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 164.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">164 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[165]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(165) * 2 + sum(//cbc:LineAmount165 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge165[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 165.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">165 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[166]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(166) * 2 + sum(//cbc:LineAmount166 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge166[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 166.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">166 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[167]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(167) * 2 + sum(//cbc:LineAmount167 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge167[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 167.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">167 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[168]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(168) * 2 + sum(//cbc:LineAmount168 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge168[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 168.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">168 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[169]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(169) * 2 + sum(//cbc:LineAmount169 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge169[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 169.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">169 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[170]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(170) * 2 + sum(//cbc:LineAmount170 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge170[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 170.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">170 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[171]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(171) * 2 + sum(//cbc:LineAmount171 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge171[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 171.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">171 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[172]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(172) * 2 + sum(//cbc:LineAmount172 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge172[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 172.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">172 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[173]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(173) * 2 + sum(//cbc:LineAmount173 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge173[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 173.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">173 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[174]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(174) * 2 + sum(//cbc:LineAmount174 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge174[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 174.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">174 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[175]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(175) * 2 + sum(//cbc:LineAmount175 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge175[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 175.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">175 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[176]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(176) * 2 + sum(//cbc:LineAmount176 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge176[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 176.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">176 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[177]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(177) * 2 + sum(//cbc:LineAmount177 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge177[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 177.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">177 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[178]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(178) * 2 + sum(//cbc:LineAmount178 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge178[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 178.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">178 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[179]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(179) * 2 + sum(//cbc:LineAmount179 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge179[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 179.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">179 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[180]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(180) * 2 + sum(//cbc:LineAmount180 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge180[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 180.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">180 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[181]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(181) * 2 + sum(//cbc:LineAmount181 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge181[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 181.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">181 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[182]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(182) * 2 + sum(//cbc:LineAmount182 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge182[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 182.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">182 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[183]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(183) * 2 + sum(//cbc:LineAmount183 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge183[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 183.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">183 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[184]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(184) * 2 + sum(//cbc:LineAmount184 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge184[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 184.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">184 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[185]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(185) * 2 + sum(//cbc:LineAmount185 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge185[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 185.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">185 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[186]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(186) * 2 + sum(//cbc:LineAmount186 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge186[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 186.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">186 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[187]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(187) * 2 + sum(//cbc:LineAmount187 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge187[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 187.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">187 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[188]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(188) * 2 + sum(//cbc:LineAmount188 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge188[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 188.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">188 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[189]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(189) * 2 + sum(//cbc:LineAmount189 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge189[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 189.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">189 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[190]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(190) * 2 + sum(//cbc:LineAmount190 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge190[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 190.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">190 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[191]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(191) * 2 + sum(//cbc:LineAmount191 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge191[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 191.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">191 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[192]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(192) * 2 + sum(//cbc:LineAmount192 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge192[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 192.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">192 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[193]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(193) * 2 + sum(//cbc:LineAmount193 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge193[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 193.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">193 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[194]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(194) * 2 + sum(//cbc:LineAmount194 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge194[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 194.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">194 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[195]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(195) * 2 + sum(//cbc:LineAmount195 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge195[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 195.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">195 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[196]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(196) * 2 + sum(//cbc:LineAmount196 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge196[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 196.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">196 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[197]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(197) * 2 + sum(//cbc:LineAmount197 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge197[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 197.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">197 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[198]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(198) * 2 + sum(//cbc:LineAmount198 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge198[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 198.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">198 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[199]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(199) * 2 + sum(//cbc:LineAmount199 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge199[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 199.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">199 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[200]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(200) * 2 + sum(//cbc:LineAmount200 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge200[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 200.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">200 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[201]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(201) * 2 + sum(//cbc:LineAmount201 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge201[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 201.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">201 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[202]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(202) * 2 + sum(//cbc:LineAmount202 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge202[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 202.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">202 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[203]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(203) * 2 + sum(//cbc:LineAmount203 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge203[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 203.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">203 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[204]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(204) * 2 + sum(//cbc:LineAmount204 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge204[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 204.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">204 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[205]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(205) * 2 + sum(//cbc:LineAmount205 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge205[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 205.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">205 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[206]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(206) * 2 + sum(//cbc:LineAmount206 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge206[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 206.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">206 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[207]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(207) * 2 + sum(//cbc:LineAmount207 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge207[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 207.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">207 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[208]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(208) * 2 + sum(//cbc:LineAmount208 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge208[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 208.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">208 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[209]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(209) * 2 + sum(//cbc:LineAmount209 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge209[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 209.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">209 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[210]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(210) * 2 + sum(//cbc:LineAmount210 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge210[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 210.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">210 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[211]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(211) * 2 + sum(//cbc:LineAmount211 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge211[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 211.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">211 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[212]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(212) * 2 + sum(//cbc:LineAmount212 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge212[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 212.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">212 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[213]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(213) * 2 + sum(//cbc:LineAmount213 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge213[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 213.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">213 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[214]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(214) * 2 + sum(//cbc:LineAmount214 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge214[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 214.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">214 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[215]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(215) * 2 + sum(//cbc:LineAmount215 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge215[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 215.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">215 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[216]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(216) * 2 + sum(//cbc:LineAmount216 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge216[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 216.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">216 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[217]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(217) * 2 + sum(//cbc:LineAmount217 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge217[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 217.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">217 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[218]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(218) * 2 + sum(//cbc:LineAmount218 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge218[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 218.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">218 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[219]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(219) * 2 + sum(//cbc:LineAmount219 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge219[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 219.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">219 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[220]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(220) * 2 + sum(//cbc:LineAmount220 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge220[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 220.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">220 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[221]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(221) * 2 + sum(//cbc:LineAmount221 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge221[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 221.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">221 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[222]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(222) * 2 + sum(//cbc:LineAmount222 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge222[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 222.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">222 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[223]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(223) * 2 + sum(//cbc:LineAmount223 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge223[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 223.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">223 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[224]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(224) * 2 + sum(//cbc:LineAmount224 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge224[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 224.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">224 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[225]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(225) * 2 + sum(//cbc:LineAmount225 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge225[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 225.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">225 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[226]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(226) * 2 + sum(//cbc:LineAmount226 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge226[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 226.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">226 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[227]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(227) * 2 + sum(//cbc:LineAmount227 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge227[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 227.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">227 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[228]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(228) * 2 + sum(//cbc:LineAmount228 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge228[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 228.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">228 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[229]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(229) * 2 + sum(//cbc:LineAmount229 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge229[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 229.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">229 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[230]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(230) * 2 + sum(//cbc:LineAmount230 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge230[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 230.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">230 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[231]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(231) * 2 + sum(//cbc:LineAmount231 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge231[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 231.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">231 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[232]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(232) * 2 + sum(//cbc:LineAmount232 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge232[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 232.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">232 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[233]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(233) * 2 + sum(//cbc:LineAmount233 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge233[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 233.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">233 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[234]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(234) * 2 + sum(//cbc:LineAmount234 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge234[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 234.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">234 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[235]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(235) * 2 + sum(//cbc:LineAmount235 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge235[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 235.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">235 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[236]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(236) * 2 + sum(//cbc:LineAmount236 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge236[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 236.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">236 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[237]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(237) * 2 + sum(//cbc:LineAmount237 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge237[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 237.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">237 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[238]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(238) * 2 + sum(//cbc:LineAmount238 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge238[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 238.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">238 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[239]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(239) * 2 + sum(//cbc:LineAmount239 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge239[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 239.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">239 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[240]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(240) * 2 + sum(//cbc:LineAmount240 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge240[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 240.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">240 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[241]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(241) * 2 + sum(//cbc:LineAmount241 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge241[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 241.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">241 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[242]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(242) * 2 + sum(//cbc:LineAmount242 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge242[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 242.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">242 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[243]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(243) * 2 + sum(//cbc:LineAmount243 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge243[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 243.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">243 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[244]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(244) * 2 + sum(//cbc:LineAmount244 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge244[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 244.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 6">244 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[245]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(245) * 2 + sum(//cbc:LineAmount245 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge245[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 245.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 0">245 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[246]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(246) * 2 + sum(//cbc:LineAmount246 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge246[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 246.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 1">246 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[247]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(247) * 2 + sum(//cbc:LineAmount247 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge247[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 247.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 2">247 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[248]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(248) * 2 + sum(//cbc:LineAmount248 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge248[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 248.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 3">248 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[249]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(249) * 2 + sum(//cbc:LineAmount249 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge249[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 249.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 4">249 </xsl:if>
  </xsl:template>
  <xsl:template match="xsl:template[250]" mode="check">
    <xsl:variable name="limit" select="xs:decimal(250) * 2 + sum(//cbc:LineAmount250 ! number(.))" xmlns:xs="http://www.w3.org/2001/XMLSchema"/>
    <xsl:if test="(exists(//cac:AllowanceCharge250[cbc:ChargeIndicator = true()]/cac:TaxCategory[normalize-space(cbc:ID) = 'Z'][cac:TaxScheme/normalize-space(upper-case(cbc:ID)) = 'VAT']) and (exists(//cac:Party/cac:PartyTaxScheme/cbc:CompanyID) or count(//cac:TaxRepresentativeParty[cbc:Amount > 250.5]) &gt;= 1)) or not(@name) or string-length(@mode) = 5">250 </xsl:if>
  </xsl:template>
</xsl:stylesheet>
