<xsl:stylesheet version="4.0" expand-text="yes"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:map="http://www.w3.org/2005/xpath-functions/map">
  <xsl:mode streamable="yes" use-accumulators="#all"/>
  <xsl:output method="text"/>

  <!-- A map from "caller -> peer" to the number of calls, kept up to
       date as the client spans stream past. -->
  <xsl:accumulator name="calls" as="map(xs:string, xs:integer)"
      initial-value="{}" streamable="yes">
    <xsl:accumulator-rule match="span[@kind = 'client']"
        select="let $call := @service || ' -> ' || @peer
                return map:put($value, $call, ($value($call) otherwise 0) + 1)"/>
  </xsl:accumulator>

  <xsl:template match="/traces">
    <xsl:apply-templates select="trace"/>
    <xsl:variable name="calls" select="accumulator-after('calls')"/>
    <xsl:for-each select="map:keys($calls)">
      <xsl:sort select="."/>
      <xsl:text>{.}: {$calls(.)}&#10;</xsl:text>
    </xsl:for-each>
  </xsl:template>

  <xsl:template match="trace"/>
</xsl:stylesheet>
