<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3"
            schematronEdition="2025">
  <sch:title>Invoice rules</sch:title>
  <sch:param name="currency" value="'EUR'"/>
  <sch:let name="tolerance" value="0.01"/>

  <sch:phase id="quick"><sch:active pattern="lines"/></sch:phase>
  <sch:phase id="full"><sch:active pattern="lines"/><sch:active pattern="totals"/></sch:phase>

  <sch:pattern id="lines">
    <sch:rule context="line">
      <sch:let name="amount" value="@quantity * @price"/>
      <sch:assert test="@quantity &gt; 0" id="LINE-1">Line <sch:value-of select="@number"/> orders no <sch:value-of select="@item"/>.</sch:assert>
      <sch:report test="@quantity &gt; 100" id="LINE-2" severity="warning">Line <sch:value-of select="@number"/> orders <sch:value-of select="@quantity"/> of one item.</sch:report>
      <sch:assert test="abs(@amount - $amount) le $tolerance" id="LINE-3">Line <sch:value-of select="@number"/>: <sch:value-of select="@amount"/> is not <sch:value-of select="$amount"/>.</sch:assert>
    </sch:rule>
  </sch:pattern>

  <sch:pattern id="totals">
    <sch:rule context="invoice">
      <sch:assert test="@currency = $currency" id="TOTAL-1" role="error">The currency is <sch:value-of select="@currency"/>, not <sch:value-of select="$currency"/>.</sch:assert>
      <sch:assert test="abs(sum(line/@amount) - @total) le $tolerance" id="TOTAL-2">The total <sch:value-of select="@total"/> is not the sum of the lines.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
