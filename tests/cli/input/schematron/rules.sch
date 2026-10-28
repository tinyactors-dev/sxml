<?xml version="1.0" encoding="UTF-8"?>
<sch:schema xmlns:sch="http://purl.oclc.org/dsdl/schematron" queryBinding="xslt3"
            schematronEdition="2025">
  <sch:title>Order rules</sch:title>
  <sch:param name="limit" value="100"/>
  <sch:phase id="totals-only"><sch:active pattern="totals"/></sch:phase>
  <sch:phase id="lines-only" when="exists(//line)"><sch:active pattern="lines"/></sch:phase>
  <sch:pattern id="lines">
    <sch:rule context="line">
      <sch:assert test="@quantity &gt; 0" id="positive-quantity">A line orders at least one <sch:value-of select="@item"/>.</sch:assert>
      <sch:report test="@quantity &gt; 10" id="large-quantity" severity="warning">Many <sch:value-of select="@item"/>: <sch:value-of select="@quantity"/>.</sch:report>
    </sch:rule>
  </sch:pattern>
  <sch:pattern id="totals">
    <sch:rule context="order">
      <sch:assert test="sum(line/@quantity) le $limit" id="total-limit" role="error">At most <sch:value-of select="$limit"/> items in an order.</sch:assert>
    </sch:rule>
  </sch:pattern>
</sch:schema>
