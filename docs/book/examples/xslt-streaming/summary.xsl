<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema">
  <xsl:mode streamable="yes" use-accumulators="#all"/>
  <xsl:output method="text"/>

  <!-- How many orders have gone past. -->
  <xsl:accumulator name="count" as="xs:integer" initial-value="0"
      streamable="yes">
    <xsl:accumulator-rule match="order" select="$value + 1"/>
  </xsl:accumulator>

  <!-- The largest total so far. -->
  <xsl:accumulator name="largest" as="xs:decimal" initial-value="0"
      streamable="yes">
    <xsl:accumulator-rule match="order/total/text()"
        select="max(($value, xs:decimal(.)))"/>
  </xsl:accumulator>

  <xsl:template match="/orders">
    <xsl:apply-templates select="order"/>
    <xsl:text>{accumulator-after('count')} orders, </xsl:text>
    <xsl:text>the largest {accumulator-after('largest')}&#10;</xsl:text>
  </xsl:template>

  <!-- Report every 25,000th order on the way. -->
  <xsl:template match="order">
    <xsl:if test="accumulator-before('count') mod 25000 eq 0">
      <xsl:message>{accumulator-before('count')} orders read</xsl:message>
    </xsl:if>
  </xsl:template>
</xsl:stylesheet>
