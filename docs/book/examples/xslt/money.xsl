<xsl:package name="money.xsl" package-version="1.0" version="4.0"
    xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
    xmlns:xs="http://www.w3.org/2001/XMLSchema"
    xmlns:money="https://example.org/money">

  <!-- The rates by which one unit of a currency is converted to euros. -->
  <xsl:variable name="money:rates" as="map(xs:string, xs:decimal)"
      select="{ 'EUR': 1.0, 'USD': 0.92 }"/>

  <xsl:function name="money:in-euros" as="xs:decimal" visibility="public">
    <xsl:param name="price" as="element(price)"/>
    <xsl:sequence select="round(xs:decimal($price) * $money:rates(string($price/@currency)), 2)"/>
  </xsl:function>

  <xsl:function name="money:format" as="xs:string" visibility="public">
    <xsl:param name="amount" as="xs:decimal"/>
    <xsl:sequence select="format-number($amount, '#,##0.00') || ' EUR'"/>
  </xsl:function>
</xsl:package>
