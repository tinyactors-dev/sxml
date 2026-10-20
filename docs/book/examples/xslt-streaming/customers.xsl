<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:mode streamable="yes" use-accumulators="#all"/>
  <xsl:output method="text"/>

  <!-- A map from each customer to the sum of their orders, kept up to
       date as the orders stream past. -->
  <xsl:accumulator name="totals" as="map(xs:string, xs:decimal)"
      initial-value="{}" streamable="yes">
    <xsl:accumulator-rule match="order/total" phase="end"
        select="let $customer := string(../customer)
                return map:put($value, $customer,
                               ($value($customer) otherwise 0) + xs:decimal(.))"/>
  </xsl:accumulator>

  <xsl:template match="/orders">
    <xsl:apply-templates/>
    <xsl:variable name="totals" select="accumulator-after('totals')"/>
    <xsl:for-each select="map:keys($totals)">
      <xsl:sort select="."/>
      <xsl:text>{.}: {$totals(.)}&#10;</xsl:text>
    </xsl:for-each>
  </xsl:template>

  <xsl:template match="order"/>
</xsl:stylesheet>
